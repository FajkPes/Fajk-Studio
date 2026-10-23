#include "fajk/pipeline.hpp"
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/media.hpp"
#include "fajk/hardware.hpp"
#include "output_safety.hpp"
#include "temporal.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace fajk {
namespace {
class EncodePool {
    struct Task{std::shared_ptr<Frame> frame;std::promise<Packet> promise;};
    std::mutex mutex_;std::condition_variable condition_;std::deque<Task> queue_;std::deque<Bytes> buffers_;bool closing_=false;
    std::vector<std::thread> threads_;EncodeOptions options_;
public:
    EncodePool(uint32_t n,EncodeOptions options):options_(options){
        try{for(uint32_t i=0;i<n;++i)threads_.emplace_back([this]{std::unique_ptr<FrameCodec> codec;while(true){Task task;{
            std::unique_lock lock(mutex_);condition_.wait(lock,[this]{return closing_||!queue_.empty();});if(queue_.empty())return;task=std::move(queue_.front());queue_.pop_front();}
            try{if(!codec)codec=makeCodec(options_.codec);auto packet=codec->encode(*task.frame,options_);
                {std::lock_guard lock(mutex_);if(!closing_&&task.frame.use_count()==1&&buffers_.size()<2)buffers_.push_back(std::move(task.frame->pixels));}
                task.promise.set_value(std::move(packet));}catch(...){task.promise.set_exception(std::current_exception());}}});}
        catch(...){close();throw;}
    }
    ~EncodePool(){close();}
    Bytes takeBuffer(){std::lock_guard lock(mutex_);if(buffers_.empty())return {};auto bytes=std::move(buffers_.front());buffers_.pop_front();return bytes;}
    void close(){ {std::lock_guard lock(mutex_);closing_=true;queue_.clear();}condition_.notify_all();for(auto& t:threads_)if(t.joinable())t.join();}
    std::future<Packet> submit(std::shared_ptr<Frame> frame){Task task{std::move(frame),{}};auto future=task.promise.get_future();{std::lock_guard lock(mutex_);queue_.push_back(std::move(task));}condition_.notify_one();return future;}
};
void cancelled(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Conversion cancelled.");}
}
Progress convert(const Path& input,const Path& output,const ConvertOptions& o,std::stop_token stop,const std::function<void(const Progress&)>& progress){
    require(o.every>=1&&o.every<=10000,"Frame sampling must be 1–10000.");require(o.memoryMiB==0||o.memoryMiB>=128,"Memory budget must be automatic or at least 128 MiB.");
    require(o.keyframeInterval<=maxFrames,"Keyframe interval must be 0–10000000.");
    require(o.similarTolerance<=16&&o.similarHoldFrames>=1&&o.similarHoldFrames<=120,"Similar-region tolerance must be 0–16 and hold 1–120 frames.");
    requireDifferentFiles(input,output);
    const auto start=std::chrono::steady_clock::now();MediaSource source(input,o.maxWidth);auto info=source.info();
    require(!o.encode.budgetHeatmap||info.image||o.maxFrameCount==1,
            "A budget heatmap requires an image or --max-frames 1.");
    require(!o.encode.budgetHeatmap||(!o.encode.lossless),
            "A budget heatmap requires lossy wavelet encoding.");
    if(o.encode.budgetHeatmap)*o.encode.budgetHeatmap={};
    auto extension=output.extension().wstring();std::transform(extension.begin(),extension.end(),extension.begin(),::towlower);
    require(extension!=L".fajk"||info.image||o.maxFrameCount==1,"Use .fajkv for video, or --max-frames 1 to save a single .fajk image.");
    const uint32_t workers=safeWorkers(info.width,info.height,o.encode.lossless,o.workers,o.memoryMiB);
    auto settings=o.encode;settings.threads=workers;if(o.autoChroma){settings.chroma420=!settings.lossless;settings.chroma422=false;}
    if(o.targetIncludesContainer&&(info.image||o.maxFrameCount==1)&&settings.targetBytes&&!settings.lossless){
        constexpr uint32_t singleFrameOverhead=128+40;
        require(settings.targetBytes>=singleFrameOverhead+256,"Single-frame target must be at least 424 bytes including the container.");
        settings.targetBytes-=singleFrameOverhead;
    }
    SequenceInfo seq{info.width,info.height,info.fpsNum,info.fpsDen,o.encode.codec,o.every,0,0};
    seq.temporal=o.temporal&&!info.image&&o.maxFrameCount!=1;
    require(!seq.temporal||!o.similarTolerance||!settings.lossless,"Similar-region approximation requires lossy encoding.");
    // Bound total codec parallelism: concurrent frames divide the same budget.
    // Low-memory/CPU configurations and single-frame exports keep one encoder.
    uint32_t frameWorkers=1;
    if(!info.image&&!settings.lossless&&workers>=6&&(!o.maxFrameCount||o.maxFrameCount>=4)){
        const auto hardware=hardwareInfo();const uint64_t available=(o.memoryMiB?std::min(o.memoryMiB,hardware.safeMiB):hardware.safeMiB)*1048576;
        const uint64_t pixels=uint64_t((info.width+63)&~63u)*((info.height+63)&~63u);
        const uint64_t reserve=uint64_t(pixelBytes(info.width,info.height))*8+pixels*24+128*1048576ull;
        const uint64_t job=uint64_t((info.width+63)&~63u)*256*(settings.lossless?32:80)+4*1048576ull;
        if(available>=reserve*2+job*workers)frameWorkers=2;
    }
    if(seq.temporal&&o.similarTolerance)frameWorkers=1;
    settings.threads=std::max(1u,workers/frameWorkers);
    SequenceWriter writer(output,seq,o.replace);EncodePool pool(frameWorkers,settings);std::deque<std::future<Packet>> pending;
    Progress state;state.duration=info.duration;state.workers=workers;
    TemporalEncoder temporal(settings,o.keyframeInterval,seq.temporal?o.similarTolerance:0,o.similarHoldFrames);uint64_t submitted=0;
    auto lastReport=start-std::chrono::milliseconds(50);
    auto report=[&](bool force=false){const auto now=std::chrono::steady_clock::now();state.elapsed=std::chrono::duration<double>(now-start).count();state.bytes=writer.bytesWritten();if(progress&&(force||now-lastReport>=std::chrono::milliseconds(50))){progress(state);lastReport=now;}};
    auto drain=[&]{while(pending.front().wait_for(std::chrono::milliseconds(30))!=std::future_status::ready)cancelled(stop);cancelled(stop);auto p=pending.front().get();pending.pop_front();if(seq.temporal&&o.similarTolerance&&p.referenceIndex==UINT64_MAX)temporal.commitKey(p,state.frames);writer.append(p);++state.frames;state.position=p.pts+p.duration;report();};
    // Keep one bounded extra batch ready. When the first frame takes longer,
    // the other encoder can continue instead of waiting idle for ordered IO.
    const uint32_t pendingLimit=frameWorkers*2;
    auto submit=[&](Frame&& f){
        cancelled(stop);if(pending.size()>=pendingLimit)drain();
        auto sourceFrame=std::make_shared<Frame>(std::move(f));
        // Reference reconstruction uses this same CPU/GPU worker budget. Finish
        // earlier asynchronous keys before using their canonical decoded pixels.
        if(seq.temporal&&o.similarTolerance)while(!pending.empty())drain();
        auto packet=seq.temporal?temporal.encode(sourceFrame,submitted):std::optional<Packet>{};++submitted;
        if(packet){std::promise<Packet> ready;auto future=ready.get_future();ready.set_value(std::move(*packet));pending.push_back(std::move(future));}
        else pending.push_back(pool.submit(std::move(sourceFrame)));
    };
    Frame current,held;bool hasHeld=false;uint64_t index=0,selected=0;int64_t origin=0,lastEnd=0;
    while(true){cancelled(stop);if(current.pixels.empty())current.pixels=pool.takeBuffer();if(!source.next(current))break;
        if(index==0)origin=current.pts;current.pts-=origin;lastEnd=current.pts+current.duration;
        if(index++%o.every)continue;
        if(hasHeld){held.duration=current.pts-held.pts;submit(std::move(held));hasHeld=false;}
        if(o.maxFrameCount&&selected>=o.maxFrameCount)break;
        held=std::move(current);hasHeld=true;++selected;
    }
    if(hasHeld){held.duration=std::max<int64_t>(1,lastEnd-held.pts);submit(std::move(held));}
    while(!pending.empty())drain();cancelled(stop);require(state.frames>0,"No frames could be read.");writer.finish();report(true);return state;
}
}
