#include "motion_temporal.hpp"
#include "temporal_coefficients.hpp"
#include "temporal_predictive.hpp"
#include "fajk/codec.hpp"
#include "fajk/compute.hpp"
#include "fajk/entropy.hpp"
#include "fajk/hardware.hpp"
#include "resample.hpp"
#include "coefficient_overrides.hpp"
#include "temporal_quality.hpp"
#include "clip_rate_control.hpp"
#include <cstdlib>
#include <iostream>
namespace fajk {
namespace {
std::vector<uint32_t> nativeRepairTiles(const Frame& source,const Frame& native,std::span<const uint32_t> damaged){
    const uint32_t sourceColumns=(source.width+31)/32,columns=(native.width+31)/32;
    std::vector<uint32_t> result;
    for(auto tile:damaged){
        const uint32_t sx=tile%sourceColumns*32,sy=tile/sourceColumns*32;
        const auto x0=uint32_t(std::max<int64_t>(0,int64_t(uint64_t(sx)*native.width/source.width)-16));
        const auto y0=uint32_t(std::max<int64_t>(0,int64_t(uint64_t(sy)*native.height/source.height)-16));
        const auto x1=uint32_t(std::min<uint64_t>(native.width,(uint64_t(std::min(source.width,sx+32))*native.width+source.width-1)/source.width+16));
        const auto y1=uint32_t(std::min<uint64_t>(native.height,(uint64_t(std::min(source.height,sy+32))*native.height+source.height-1)/source.height+16));
        for(uint32_t y=y0/32;y<=(y1-1)/32;++y)for(uint32_t x=x0/32;x<=(x1-1)/32;++x)result.push_back(y*columns+x);
    }
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());
    uint64_t area=0;for(auto tile:result)area+=uint64_t(std::min(32u,native.width-tile%columns*32))*std::min(32u,native.height-tile/columns*32);
    if(result.size()>32||area*8>uint64_t(native.width)*native.height)result.clear();return result;
}
void repairNative(Frame& target,const Frame& independent,std::span<const uint32_t> tiles){
    const auto columns=(target.width+31)/32;
    for(auto tile:tiles){const auto x0=tile%columns*32,y0=tile/columns*32,w=std::min(32u,target.width-x0),h=std::min(32u,target.height-y0);
        for(uint32_t y=0;y<h;++y){const auto at=(size_t(y0+y)*target.width+x0)*4;std::copy_n(independent.pixels.data()+at,size_t(w)*4,target.pixels.data()+at);}}
}
}
struct MotionTemporalEncoder::State {
    EncodeOptions options;uint32_t interval=120,tolerance=6,hold=10,depth=0;
    uint64_t rootIndex=UINT64_MAX,lastIndex=UINT64_MAX;int64_t rootPts=0;
    std::unique_ptr<FrameCodec> codec;std::shared_ptr<const MotionReference> reference,root;
    Frame source;std::shared_ptr<const Frame> witness,rootWitness;Bytes ages;uint32_t repeatVersion=motionTemporalVersion;size_t rootBytes=0;
    Frame presentation;
    std::unique_ptr<CoefficientTemporalEncoder> exact;
    bool qualityTrace=false;
    bool stabilizeQuantizer=false;ClipRateControl rateControl;
};
MotionTemporalEncoder::MotionTemporalEncoder(EncodeOptions options,uint32_t interval,uint32_t tolerance,uint32_t hold,bool stabilizeQuantizer):state_(std::make_unique<State>()){
    require(!options.lossless&&tolerance<=16&&hold>=1&&hold<=120,"Invalid max-compression temporal options.");
    require(!options.reconstruction441.protectText,"Max compression does not support text protection.");
    auto& s=*state_;s.options=std::move(options);s.interval=interval;s.tolerance=tolerance;s.hold=hold;s.codec=makeCodec(s.options.codec);
    s.stabilizeQuantizer=stabilizeQuantizer;
    s.codec->setThreads(s.options.threads);s.codec->setComputeBackend(s.options.backend);
    s.qualityTrace=std::getenv("FAJK_MOTION_QUALITY_TRACE")!=nullptr;
}
MotionTemporalEncoder::~MotionTemporalEncoder()=default;
Packet MotionTemporalEncoder::encode(Frame source,uint64_t index){
    return encodePrepared(std::move(source),{}, {},index);
}
Packet MotionTemporalEncoder::encodePrepared(Frame source,Packet prepared,CoefficientEncodeCapture capture,uint64_t index){
    auto& s=*state_;source.validate();
    ScopedComputeBackend compute(s.options.backend);
    if(s.lastIndex!=UINT64_MAX&&source.width==s.source.width&&source.height==s.source.height&&source.pixels==s.source.pixels){
        ByteWriter out;out.u32(0x334b4a46);out.u8(uint8_t(s.repeatVersion));out.u8(2);out.u16(0);Packet packet{std::move(out.data),source.pts,source.duration};packet.referenceIndex=s.lastIndex;return packet;
    }
    // Zero means automatic, as in FrameCodec. Clamping zero to one here also
    // capped every nested key-frame encoder to one worker for the whole scope.
    // Exact holds need no new working buffers or worker/memory validation.
    const auto workers=safeWorkers(source.width,source.height,false,s.options.threads);
    ScopedWorkerBudget workerBudget(workers);
    const bool refresh=!s.reference||source.width!=s.source.width||source.height!=s.source.height||source.pts-s.rootPts>=temporalMaxKeyDuration||(s.interval&&index-s.rootIndex>=s.interval)||!predictiveOpaque(source);
    // An actual independent reconstruction is the quality oracle and the
    // already-encoded fallback. Motion cost/quantizer step cannot establish
    // the quality of either a reference or its final display filters.
    ScopedCoefficientEncodeCapture captureScope(&capture);
    Packet key;
    if(prepared.data.empty()){
        auto rate=s.stabilizeQuantizer?s.rateControl.begin(source,s.options):WaveletRateControl{};ScopedWaveletRateControl rateScope(s.stabilizeQuantizer?&rate:nullptr);
        key=s.codec->encode(source,s.options);if(s.stabilizeQuantizer)s.rateControl.finish(key);
    }else key=std::move(prepared);
    require(!key.reference&&key.referenceIndex==UINT64_MAX&&key.pts==source.pts&&key.duration==source.duration,"Invalid prepared motion frame.");
    Frame independentDisplay;
    auto independent=readMotionRoot(key,source.width,source.height,workers,s.options.backend==ComputeBackend::Cpu?&independentDisplay:nullptr);
    // Submit the display reconstruction before the CPU alternative search.
    // Deferring this until acceptance regressed the bounded GPU pipeline.
    if(independentDisplay.pixels.empty())independentDisplay=s.codec->decode(key,source.width,source.height);
    // Restart a short seek chain from the same key when useful, rather than
    // injecting a new independent reconstruction/noise pattern every 16 frames.
    // If the old key predicts poorly, the size check below refreshes normally.
    if(!refresh&&s.depth>=coefficientSeekDepth-1){s.reference=s.root;s.witness=s.rootWitness;s.ages.clear();s.lastIndex=s.rootIndex;s.depth=0;}
    if(!s.exact||refresh)s.exact=std::make_unique<CoefficientTemporalEncoder>(workers,s.interval,true,true,s.options.speed==EncodeSpeed::MoreQuality);
    const auto& exactAlternative=s.exact->preview(key,index);
    if(!refresh&&exactAlternative.data.size()>400&&independent&&independent->image.width==s.reference->image.width&&independent->image.height==s.reference->image.height){
        auto working=resamplePhoto(source,s.reference->image.width,s.reference->image.height,workers,{},2);
        // Keep the ordinary 0..6 range at the original fine quantizer. A
        // universally steeper curve lost too much low-rate detail; ramp the
        // extra rate saving into the explicitly aggressive 7..12 range only.
        const float aggressive=std::clamp((float(s.tolerance)-6.f)/6.f,0.f,1.f);
        const auto cap=s.options.targetBytes?size_t(s.options.targetBytes):s.rootBytes;
        // A coarse intra quantizer is not a safe residual error allowance.
        // At low bitrates the old multiplication reached steps above 200,
        // leaving motion seams and colored reference errors uncorrected.
        // Scale the ceiling with native sample density. A universal fine cap
        // caused low-budget videos to fall back to a new noisy key too often.
        // Bound the allowance in proportion to native samples per byte;
        // even the sparsest budgets cannot reach the previous 200+ steps.
        const auto qualityCeiling=uint32_t(std::clamp(std::lround(2.*double(working.width)*working.height/double(cap)),64l,128l));
        const auto step=uint32_t(std::clamp(std::lround(s.reference->residualStep*(.20f+(.07f+.05f*aggressive)*s.tolerance)),1l,long(qualityCeiling*3/4)));
        const auto totalLimit=std::min(cap-1,(std::min(key.data.size(),exactAlternative.data.size())*98-1)/100);
        const auto maximumResidual=totalLimit>12?totalLimit-12:0;
        const auto maximumStep=s.tolerance?std::min(qualityCeiling,step*2):step;
        // Low-rate references cannot recover all lost texture every frame.
        // Retain bounded luma error there to avoid unnecessary key refreshes;
        // chroma error is still corrected, never accumulated in the residual.
        const auto carryLimit=std::min(32u,8u+(qualityCeiling>72?qualityCeiling-72:0));
        auto accept=[&](PredictiveTrial& trial)->std::optional<Packet>{
            if(trial.overBudget||trial.image.pixels.empty()||trial.packet.data.size()>maximumResidual)return {};
            // Canonical reference pixels are intentionally not the displayed
            // image. Judge the complete presentation below, after the same
            // reconstruction/scale path that playback will use.
            auto next=std::make_shared<MotionReference>(MotionReference{std::move(trial.image),s.reference->settings,s.reference->budget,s.reference->surfaces,s.reference->contours,s.reference->scaled,s.reference->residualStep});
            auto displayed=renderMotionReference(*next,source.pts,source.duration,source.width,source.height,workers);
            Bytes repairs;
            const auto& independentPixels=independentDisplay;
            if(displayed.pixels!=independentPixels.pixels){
                const auto* history=&s.presentation;
                const auto quality=compareTemporalQuality(source,displayed,independentPixels,workers,&s.source,history,!s.qualityTrace);
                if(s.qualityTrace)std::cerr<<"MOTION_QUALITY,"<<index<<','<<unsigned(trial.packet.data[6])<<','<<trial.selectedStep<<','<<trial.packet.data.size()<<','
                    <<quality.candidate.rgb<<','<<quality.independent.rgb<<','<<quality.candidate.chroma<<','<<quality.independent.chroma<<','
                    <<quality.candidate.gradient<<','<<quality.independent.gradient<<','<<quality.candidate.flicker<<','<<quality.independent.flicker<<','
                    <<quality.damagedTiles<<','<<quality.hueTiles<<','<<quality.rgbTiles<<','<<quality.chromaTiles<<','<<quality.gradientTiles<<','<<quality.accepted()<<'\n';
                if(!quality.accepted()){
                    // Repair a few bad regions rather than discarding good
                    // prediction throughout the frame. The final complete
                    // display must still pass every existing quality gate.
                    if(!quality.damagedTiles||trial.packet.data.size()>=maximumResidual*2/3)return {};
                    std::vector<uint32_t> damaged;
                    const auto full=compareTemporalQuality(source,displayed,independentPixels,workers,&s.source,history,false,&damaged);
                    if(!full.measuredFullFrame||full.damagedTiles>4)return {};
                    const auto tiles=nativeRepairTiles(source,next->image,damaged);if(tiles.empty())return {};
                    repairs=encodeMotionRepairs(next->image,independent->image,tiles,workers);
                    if(repairs.size()>maximumResidual-trial.packet.data.size())return {};
                    repairNative(next->image,independent->image,tiles);
                    displayed=renderMotionReference(*next,source.pts,source.duration,source.width,source.height,workers);
                    if(!compareTemporalQuality(source,displayed,independentPixels,workers,&s.source,history,true).accepted())return {};
                }
            }
            ByteWriter out;out.u32(0x334b4a46);out.u8(motionTemporalVersion);out.u8(repairs.empty()?1:3);out.u16(0);out.u32(uint32_t(trial.packet.data.size()));out.bytes(trial.packet.data);out.bytes(repairs);
            Packet packet{std::move(out.data),source.pts,source.duration};packet.referenceIndex=s.lastIndex;
            s.reference=std::move(next);s.witness=std::make_shared<Frame>(std::move(trial.witness));s.ages=std::move(trial.ages);s.lastIndex=index;++s.depth;
            s.source=std::move(source);s.presentation=std::move(displayed);s.repeatVersion=motionTemporalVersion;s.exact->discardPreview();return packet;
        };
        auto tryFilters=[&](PredictiveTrial& trial)->std::optional<Packet>{
            if(auto accepted=accept(trial))return accepted;
            if(!trial.overBudget&&!trial.packet.data.empty()){
                // Both filters have the same byte cost. Select by reconstructed
                // source quality, not by assuming adjacent taps always win.
                trial.packet.data[6]=15;
                trial.image=decodePredictiveTrial(trial.packet,s.reference->image,workers);
                if(auto accepted=accept(trial))return accepted;
            }
            return {};
        };
        MotionResidualSettings residualSettings;
        residualSettings.backend=s.options.backend;residualSettings.tolerance=s.tolerance;residualSettings.step=step;residualSettings.maximumStep=maximumStep;
        residualSettings.holdFrames=s.hold;residualSettings.errorCarry=std::min(carryLimit,s.tolerance*3);residualSettings.maximumBytes=maximumResidual;
        auto trial=trialMotion(working,*s.witness,s.reference->image,workers,residualSettings,s.ages);
        const auto fineStep=std::max(1u,trial.selectedStep/2);
        const bool headroom=!trial.overBudget&&!trial.packet.data.empty()&&trial.packet.data.size()<maximumResidual*2/3&&fineStep<trial.selectedStep;
        if(auto accepted=tryFilters(trial))return std::move(*accepted);
        if(headroom){
            auto finerSettings=residualSettings;finerSettings.step=fineStep;finerSettings.maximumStep=0;finerSettings.errorCarry=0;
            auto finer=trialMotion(working,*s.witness,s.reference->image,workers,finerSettings,s.ages);
            if(auto accepted=tryFilters(finer))return std::move(*accepted);
        }
    }
    // A rejected motion proposal must not buy its size saving with artifacts.
    // Exact coefficient recoding can still save bytes without changing any
    // independent-frame pixels or introducing another lossy reconstruction.
    auto result=s.exact->commitPreview();
    if(independent){s.reference=std::make_shared<MotionReference>(std::move(*independent));s.witness=std::make_shared<Frame>(resamplePhoto(source,s.reference->image.width,s.reference->image.height,workers,{},2));}else{s.reference.reset();s.witness={};}
    s.presentation=std::move(independentDisplay);
    if(result.referenceIndex!=UINT64_MAX){
        s.ages.clear();s.lastIndex=index;s.depth=s.exact->encodedDepth();s.source=std::move(source);s.repeatVersion=coefficientTemporalVersion;return result;
    }
    s.root=s.reference;s.rootWitness=s.witness;
    s.rootPts=source.pts;s.source=std::move(source);s.ages.clear();s.rootIndex=s.lastIndex=index;s.depth=0;s.rootBytes=key.data.size();s.repeatVersion=coefficientTemporalVersion;return result;
}
}
