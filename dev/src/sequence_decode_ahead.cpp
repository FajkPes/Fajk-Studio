#include "sequence_decode_ahead.hpp"
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/hardware.hpp"
#include "frame_pool.hpp"
#include "gpu_activity_delta.hpp"
#include "decode_throughput.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace fajk {
struct SequenceDecodeAhead::Impl {
    std::mutex mutex;std::condition_variable wake;
    bool closing=false,running=false;
    std::exception_ptr failure;
    uint32_t lanes=1,step=1,chunk=1,window=1;
    uint64_t revision=0,first=0,last=0,count=0,claimed=0,consumed=0;
    SequenceDecodeSettings settings;
    DecodeThroughput throughput;
    std::shared_ptr<SharedWorkerBudget> sharedBudget;
    std::vector<std::optional<SequenceDecodeResult>> ready;
    std::vector<std::jthread> workers;
    explicit Impl(uint32_t n):lanes(n){
        require(n>=1&&n<=4,"Decode lanes must be 1–4.");
        try{for(uint32_t lane=0;lane<n;++lane)workers.emplace_back([this,lane]{try{run(lane);}catch(...){fail(std::current_exception());}});}
        catch(...){close();throw;}
    }
    ~Impl(){close();}
    void fail(std::exception_ptr error){
        {std::lock_guard lock(mutex);if(!failure)failure=std::move(error);running=false;++revision;}
        wake.notify_all();
    }
    void close(){
        {std::lock_guard lock(mutex);closing=true;running=false;++revision;ready.clear();}
        wake.notify_all();for(auto& worker:workers)if(worker.joinable())worker.join();
    }
    void run(uint32_t lane){
        std::unique_ptr<SequenceReader> reader;std::unique_ptr<FrameCodec> codec;
        Path path;uint64_t content=UINT64_MAX;FramePool frames;
        for(;;){
            SequenceDecodeSettings work;std::shared_ptr<SharedWorkerBudget> group;uint64_t jobRevision=0,ordinal=0,begin=0,end=0;uint32_t stride=1,jobs=0;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return closing||(running&&claimed<count&&claimed<consumed+window&&(!settings.epoch||*settings.epoch==settings.generation));});
                if(closing)return;
                work=settings;group=sharedBudget;jobRevision=revision;ordinal=claimed;begin=first;end=last;stride=step;
                jobs=uint32_t(std::min<uint64_t>({chunk,count-claimed,consumed+window-claimed}));claimed+=jobs;
            }
            for(uint32_t at=0;at<jobs;++at){
                {std::lock_guard lock(mutex);if(closing)return;if(jobRevision!=revision||(work.epoch&&*work.epoch!=work.generation))break;
                    throughput.begin(DecodeThroughput::Clock::now());}
                SequenceDecodeResult result;result.frame=std::min(end,begin+(ordinal+at)*stride);result.generation=work.generation;
                try{
                    ScopedSharedWorkerBudget binding(group.get());
                    if(path!=work.path||content!=work.contentRevision){
                        auto opened=std::make_unique<SequenceReader>(work.path);auto fresh=makeCodec(CodecId::Fajk);
                        reader=std::move(opened);codec=std::move(fresh);path=work.path;content=work.contentRevision;
                    }
                    const auto& info=reader->info();require(info.width==work.width&&info.height==work.height,"Decode-ahead dimensions changed.");
                    require(result.frame<info.count,"Decode-ahead frame index is out of range.");
                    // The sum of lane budgets never exceeds the caller's total.
                    const auto total=workerLimit(work.threads),share=group?total:total/lanes+(lane<total%lanes?1u:0u);
                    require(share>0,"Not enough CPU workers for decode lanes.");
                    const auto laneWorkers=safeWorkers(info.width,info.height,false,share,work.memoryMiB?work.memoryMiB/lanes:0);
                    codec->setThreads(laneWorkers);codec->setComputeBackend(work.backend);codec->setDeblocking(work.deblocking);codec->setSharpening(work.sharpening);
                    const auto cacheMiB=work.memoryMiB?std::min<uint64_t>(256,work.memoryMiB/8):256;
                    codec->setCacheBudgetBytes(size_t(cacheMiB/lanes)*1024*1024);
                    const auto packet=reader->read(result.frame);result.image=frames.acquire(info.width,info.height,packet.pts,packet.duration);
                    const auto before=gpuActivity();const auto start=std::chrono::steady_clock::now();
                    codec->decodeInto(packet,info.width,info.height,result.image->pixels);
                    result.decodeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                    accumulateGpu(result.gpu,gpuActivity(),before);
                }catch(const std::exception& error){result.image.reset();result.error=error.what();}
                catch(...){result.image.reset();result.error="Decode-ahead worker failed.";}
                {std::lock_guard lock(mutex);if(closing)return;if(jobRevision!=revision)break;
                    throughput.finish(DecodeThroughput::Clock::now(),result.error.empty()&&bool(result.image));
                    ready[(ordinal+at)%window]=std::move(result);
                }
                wake.notify_all();
            }
        }
    }
};
SequenceDecodeAhead::SequenceDecodeAhead(uint32_t lanes):impl_(std::make_unique<Impl>(lanes)){}
SequenceDecodeAhead::~SequenceDecodeAhead()=default;
void SequenceDecodeAhead::start(SequenceDecodeSettings settings,uint64_t first,uint64_t last,uint32_t step,uint32_t window,uint32_t chunk){
    require(!settings.path.empty()&&first<=last&&last<maxFrames&&step>0&&window>=1&&window<=16&&chunk>=1&&chunk<=window,"Invalid decode-ahead window.");
    pixelBytes(settings.width,settings.height);
    require(settings.memoryMiB==0||settings.memoryMiB>=128u*impl_->lanes,"Decode-ahead memory budget is too small.");
    require(workerLimit(settings.threads)>=impl_->lanes,"Not enough CPU workers for decode lanes.");
    std::vector<std::optional<SequenceDecodeResult>> ready(window);
    {std::lock_guard lock(impl_->mutex);require(!impl_->closing,"Decode-ahead is closed.");
        if(impl_->failure)std::rethrow_exception(impl_->failure);
        if(settings.borrowCpuWorkers){
            if(!impl_->sharedBudget||impl_->settings.threads!=settings.threads)impl_->sharedBudget=std::make_shared<SharedWorkerBudget>(workerLimit(settings.threads),impl_->lanes);
        }else impl_->sharedBudget.reset();
        ++impl_->revision;impl_->throughput={};impl_->settings=std::move(settings);impl_->first=first;impl_->last=last;
        impl_->count=(last-first)/step+1+uint64_t(impl_->settings.includeLast&&(last-first)%step);
        impl_->step=step;impl_->window=window;impl_->chunk=chunk;impl_->claimed=impl_->consumed=0;
        impl_->ready=std::move(ready);impl_->running=true;
    }
    impl_->wake.notify_all();
}
void SequenceDecodeAhead::cancel(){
    {std::lock_guard lock(impl_->mutex);++impl_->revision;impl_->running=false;for(auto& result:impl_->ready)result.reset();}
    impl_->wake.notify_all();
}
std::optional<SequenceDecodeResult> SequenceDecodeAhead::take(uint64_t frame,uint64_t generation,const std::atomic<uint64_t>* epoch){
    std::optional<SequenceDecodeResult> result;
    {std::unique_lock lock(impl_->mutex);
        if(impl_->failure)std::rethrow_exception(impl_->failure);
        auto cancelled=[&]{return impl_->closing||!impl_->running||impl_->settings.generation!=generation||(epoch&&*epoch!=generation)||(impl_->settings.epoch&&*impl_->settings.epoch!=generation);};
        if(cancelled())return {};
        require(impl_->consumed<impl_->count&&frame==std::min(impl_->last,impl_->first+impl_->consumed*impl_->step),"Decode-ahead delivery must be ordered.");
        const auto revision=impl_->revision;
        while(!cancelled()&&revision==impl_->revision&&!impl_->ready[impl_->consumed%impl_->window])impl_->wake.wait_for(lock,std::chrono::milliseconds(20));
        if(impl_->failure)std::rethrow_exception(impl_->failure);
        if(cancelled()||revision!=impl_->revision)return {};
        auto& ready=impl_->ready[impl_->consumed%impl_->window];result=std::move(ready);ready.reset();++impl_->consumed;
        const auto speed=impl_->throughput.sample(DecodeThroughput::Clock::now());
        result->throughputMs=speed.msPerFrame;result->throughputFrames=speed.frames;
        if(impl_->consumed==impl_->count)impl_->running=false;
    }
    impl_->wake.notify_all();return result;
}
}
