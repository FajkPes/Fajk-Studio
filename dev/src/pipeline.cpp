#include "fajk/pipeline.hpp"
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/media.hpp"
#include "fajk/hardware.hpp"
#include "fajk/entropy.hpp"
#include "output_safety.hpp"
#include "temporal.hpp"
#include "temporal_coefficients.hpp"
#include "motion_temporal.hpp"
#include "coefficient_overrides.hpp"
#include "clip_rate_control.hpp"
#include "gpu_activity_delta.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace fajk {
namespace {
class EncodePool {
public:
    struct Encoded {Packet packet;CoefficientEncodeCapture coefficients;GpuActivity gpu;};
private:
    struct Task{std::shared_ptr<Frame> frame;std::promise<Encoded> promise;};
    std::mutex mutex_;std::condition_variable condition_;std::deque<Task> queue_;std::deque<Bytes> buffers_;bool closing_=false;
    std::vector<std::thread> threads_;EncodeOptions options_;bool capture_,stabilize_;
    SharedWorkerBudget* shared_;
public:
    EncodePool(uint32_t n,EncodeOptions options,bool capture=false,bool stabilize=false,SharedWorkerBudget* shared=nullptr):options_(options),capture_(capture),stabilize_(stabilize),shared_(shared){
        require(!stabilize||n==1,"Clip rate control needs one ordered producer.");
        try{for(uint32_t i=0;i<n;++i)threads_.emplace_back([this]{std::unique_ptr<FrameCodec> codec;ClipRateControl rateControl;while(true){Task task;{
            std::unique_lock lock(mutex_);condition_.wait(lock,[this]{return closing_||!queue_.empty();});if(queue_.empty())return;task=std::move(queue_.front());queue_.pop_front();}
            try{ScopedSharedWorkerBudget binding(shared_);const auto gpuBefore=gpuActivity();if(!codec)codec=makeCodec(options_.codec);CoefficientEncodeCapture coefficients;ScopedCoefficientEncodeCapture capture(capture_?&coefficients:nullptr);
                auto rate=stabilize_?rateControl.begin(*task.frame,options_):WaveletRateControl{};ScopedWaveletRateControl rateScope(stabilize_?&rate:nullptr);
                auto packet=codec->encode(*task.frame,options_);if(stabilize_)rateControl.finish(packet);
                {std::lock_guard lock(mutex_);if(!closing_&&task.frame.use_count()==1&&buffers_.size()<2)buffers_.push_back(std::move(task.frame->pixels));}
                GpuActivity gpu;accumulateGpu(gpu,gpuActivity(),gpuBefore);
                task.promise.set_value({std::move(packet),std::move(coefficients),gpu});}catch(...){task.promise.set_exception(std::current_exception());}}});}
        catch(...){close();throw;}
    }
    ~EncodePool(){close();}
    Bytes takeBuffer(){std::lock_guard lock(mutex_);if(buffers_.empty())return {};auto bytes=std::move(buffers_.front());buffers_.pop_front();return bytes;}
    void close(){ {std::lock_guard lock(mutex_);closing_=true;queue_.clear();}condition_.notify_all();for(auto& t:threads_)if(t.joinable())t.join();}
    std::future<Encoded> submit(std::shared_ptr<Frame> frame){Task task{std::move(frame),{}};auto future=task.promise.get_future();{std::lock_guard lock(mutex_);queue_.push_back(std::move(task));}condition_.notify_one();return future;}
};
void cancelled(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Conversion cancelled.");}
}
Progress convert(const Path& input,const Path& output,const ConvertOptions& o,std::stop_token stop,const std::function<void(const Progress&)>& progress){
    require(o.every>=1&&o.every<=10000,"Frame sampling must be 1–10000.");require(o.memoryMiB==0||o.memoryMiB>=128,"Memory budget must be automatic or at least 128 MiB.");
    require(o.keyframeInterval<=maxFrames,"Keyframe interval must be 0–10000000.");
    require(o.similarTolerance<=16&&o.similarHoldFrames>=1&&o.similarHoldFrames<=120,"Similar-region tolerance must be 0–16 and hold 1–120 frames.");
    require(!o.coefficientTemporal||o.temporal,"Coefficient temporal coding requires temporal mode.");
    require(!o.maxCompressionTemporal||(o.temporal&&!o.coefficientTemporal&&!o.encode.lossless),"Max compression requires lossy temporal mode without coefficient recoding.");
    require(!o.coefficientTemporal||o.similarTolerance==0,"Coefficient temporal coding preserves independent pixels and requires zero similarity tolerance.");
    requireDifferentFiles(input,output);
    const auto start=std::chrono::steady_clock::now();const auto gpuStart=gpuActivity();GpuActivity preparationGpu;MediaSource source(input,o.maxWidth);auto info=source.info();
    require(!o.encode.budgetHeatmap||info.image||o.maxFrameCount==1,
            "A budget heatmap requires an image or --max-frames 1.");
    require(!o.encode.budgetHeatmap||(!o.encode.lossless),
            "A budget heatmap requires lossy wavelet encoding.");
    if(o.encode.budgetHeatmap)*o.encode.budgetHeatmap={};
    auto extension=output.extension().wstring();std::transform(extension.begin(),extension.end(),extension.begin(),::towlower);
    require(extension!=L".fajk"||info.image||o.maxFrameCount==1,"Use .fajkv for video, or --max-frames 1 to save a single .fajk image.");
    const uint32_t workers=safeWorkers(info.width,info.height,o.encode.lossless,o.workers,o.memoryMiB);
    auto settings=o.encode;settings.threads=workers;if(o.autoChroma){settings.chroma420=!info.image&&!settings.lossless;settings.chroma422=false;}
    if(o.targetIncludesContainer&&(info.image||o.maxFrameCount==1)&&settings.targetBytes&&!settings.lossless){
        constexpr uint32_t singleFrameOverhead=128+40;
        require(settings.targetBytes>=singleFrameOverhead+256,"Single-frame target must be at least 424 bytes including the container.");
        settings.targetBytes-=singleFrameOverhead;
    }
    SequenceInfo seq{info.width,info.height,info.fpsNum,info.fpsDen,o.encode.codec,o.every,0,0};
    seq.temporal=o.temporal&&!info.image&&o.maxFrameCount!=1;
    seq.temporalChains=seq.temporal&&(o.coefficientTemporal||o.maxCompressionTemporal);
    require(!seq.temporal||!o.maxCompressionTemporal||!settings.reconstruction441.protectText,
            "Max compression does not support text protection. Disable text protection or choose coefficient temporal coding.");
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
    const bool motionAhead=seq.temporal&&o.maxCompressionTemporal&&o.overlapMotionPreparation&&frameWorkers==2;
    if(seq.temporal&&(o.similarTolerance||o.maxCompressionTemporal))frameWorkers=1;
    const uint32_t coefficientWorkers=seq.temporal&&o.coefficientTemporal?(workers<3?workers:std::max(1u,std::min(2u,workers/3))):0;
    settings.threads=coefficientWorkers&&workers<3?workers:std::max(1u,(workers>coefficientWorkers?workers-coefficientWorkers:1)/frameWorkers);
    auto motionSettings=settings;
    if(motionAhead){settings.threads=std::max(1u,std::min(2u,workers/3));motionSettings.threads=workers-settings.threads;}
    std::unique_ptr<SharedWorkerBudget> shared;
    if(motionAhead&&o.borrowMotionWorkers){shared=std::make_unique<SharedWorkerBudget>(workers,2);settings.threads=motionSettings.threads=workers;}
    ScopedSharedWorkerBudget binding(shared.get());
    SequenceWriter writer(output,seq,o.replace);EncodePool pool(frameWorkers,settings,motionAhead,motionAhead&&o.stabilizeTemporalQuantizer,shared.get());std::deque<std::future<EncodePool::Encoded>> pending;
    std::deque<std::shared_ptr<Frame>> motionSources;std::shared_ptr<const Frame> previousMotionSource;
    Progress state;state.duration=info.duration;state.workers=workers;state.preparationWorkers=motionAhead?settings.threads:0;state.sharedWorkers=bool(shared);
    TemporalEncoder temporal(settings,o.keyframeInterval,seq.temporal?o.similarTolerance:0,o.similarHoldFrames);uint64_t submitted=0;
    // Forced root restarts and producer coefficient capture regressed both
    // encode time and size on moving video. Keep recent-reference chains;
    // Max separately uses its source-checked root/recent alternative search.
    CoefficientTemporalEncoder coefficients(coefficientWorkers,o.keyframeInterval,true);
    std::unique_ptr<MotionTemporalEncoder> motion;
    if(seq.temporal&&o.maxCompressionTemporal)motion=std::make_unique<MotionTemporalEncoder>(motionSettings,o.keyframeInterval,o.similarTolerance,o.similarHoldFrames,o.stabilizeTemporalQuantizer);
    std::shared_ptr<const Frame> previousCoefficientSource;
    Packet previousCoefficientPacket;
    uint64_t coefficientRepeatReference=UINT64_MAX;
    auto lastReport=start-std::chrono::milliseconds(50);
    auto report=[&](bool force=false){const auto now=std::chrono::steady_clock::now();state.elapsed=std::chrono::duration<double>(now-start).count();state.bytes=writer.bytesWritten();state.gpu=preparationGpu;accumulateGpu(state.gpu,gpuActivity(),gpuStart);if(progress&&(force||now-lastReport>=std::chrono::milliseconds(50))){progress(state);lastReport=now;}};
    auto drain=[&]{
        while(pending.front().wait_for(std::chrono::milliseconds(30))!=std::future_status::ready)cancelled(stop);cancelled(stop);
        auto encoded=pending.front().get();pending.pop_front();accumulateGpu(preparationGpu,encoded.gpu);auto p=std::move(encoded.packet);
        if(motionAhead){auto frame=std::move(motionSources.front());motionSources.pop_front();p=motion->encodePrepared(std::move(*frame),std::move(p),std::move(encoded.coefficients),state.frames);cancelled(stop);}
        else if(seq.temporal&&o.coefficientTemporal)p=coefficients.encode(std::move(p),state.frames);
        else if(seq.temporal&&o.similarTolerance&&p.referenceIndex==UINT64_MAX)temporal.commitKey(p,state.frames);
        writer.append(p);if(seq.temporal&&o.coefficientTemporal){previousCoefficientPacket=p;coefficientRepeatReference=p.data.size()==8&&p.data[4]==coefficientTemporalVersion&&p.data[5]==2?p.referenceIndex:state.frames;}++state.frames;state.position=p.pts+p.duration;report();
    };
    // Keep one bounded extra batch ready. When the first frame takes longer,
    // the other encoder can continue instead of waiting idle for ordered IO.
    const uint32_t pendingLimit=seq.temporal&&o.coefficientTemporal&&workers<3?1:frameWorkers*2;
    auto submit=[&](Frame&& f){
        cancelled(stop);if(pending.size()>=pendingLimit)drain();
        if(motion&&!motionAhead){auto packet=motion->encode(std::move(f),submitted++);cancelled(stop);writer.append(packet);++state.frames;state.position=packet.pts+packet.duration;report();return;}
        auto sourceFrame=std::make_shared<Frame>(std::move(f));
        if(motionAhead){
            const bool repeat=previousMotionSource&&sourceFrame->width==previousMotionSource->width&&sourceFrame->height==previousMotionSource->height&&sourceFrame->pixels==previousMotionSource->pixels;
            previousMotionSource=sourceFrame;motionSources.push_back(sourceFrame);++submitted;
            if(repeat){std::promise<EncodePool::Encoded> ready;auto future=ready.get_future();ready.set_value({});pending.push_back(std::move(future));}
            else pending.push_back(pool.submit(std::move(sourceFrame)));return;
        }
        if(seq.temporal&&o.coefficientTemporal){
            if(previousCoefficientSource&&sourceFrame->width==previousCoefficientSource->width&&sourceFrame->height==previousCoefficientSource->height&&sourceFrame->pixels==previousCoefficientSource->pixels){
                // Exact source repeats need neither a transform nor another
                // entropy pass. Finish preceding outputs before reusing their
                // fixed reference. A long hold never grows the dependency chain.
                while(!pending.empty())drain();
                auto repeat=previousCoefficientPacket;repeat.pts=sourceFrame->pts;repeat.duration=sourceFrame->duration;
                {ByteWriter out;out.u32(0x334b4a46);out.u8(coefficientTemporalVersion);out.u8(2);out.u16(0);repeat.data=std::move(out.data);repeat.referenceIndex=coefficientRepeatReference;}
                writer.append(repeat);previousCoefficientPacket=std::move(repeat);previousCoefficientSource=std::move(sourceFrame);
                ++submitted;++state.frames;state.position=previousCoefficientPacket.pts+previousCoefficientPacket.duration;report();return;
            }
            previousCoefficientSource=sourceFrame;
        }
        // Reference reconstruction uses this same CPU/GPU worker budget. Finish
        // earlier asynchronous keys before using their canonical decoded pixels.
        if(seq.temporal&&o.similarTolerance)while(!pending.empty())drain();
        auto packet=seq.temporal&&!o.coefficientTemporal?temporal.encode(sourceFrame,submitted):std::optional<Packet>{};++submitted;
        if(packet){std::promise<EncodePool::Encoded> ready;auto future=ready.get_future();ready.set_value({std::move(*packet),{}});pending.push_back(std::move(future));}
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
