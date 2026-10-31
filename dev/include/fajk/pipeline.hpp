#pragma once
#include "types.hpp"
#include "compute.hpp"
#include <atomic>
#include <functional>
#include <stop_token>
namespace fajk {
struct ConvertOptions {
    EncodeOptions encode;
    uint32_t every=1,maxWidth=0,workers=0;
    uint64_t maxFrameCount=0,memoryMiB=0; // 0: automatic, bounded by available physical RAM
    bool replace=false;
    bool autoChroma=true; // video 4:2:0, still images 4:4:4
    bool targetIncludesContainer=false; // Single-frame CLI limits include header/index; UI supplies a packet limit.
    bool temporal=false; // Opt-in adaptive keyframes and exact source differences.
    bool coefficientTemporal=false; // Recodes quantized coefficients without changing any decoded pixels.
    bool maxCompressionTemporal=false; // Motion/affine prediction, bounded approximate skips and quantized residuals.
    bool overlapMotionPreparation=true; // Bounded producer stage; still obeys the shared worker/RAM budget.
    bool borrowMotionWorkers=false; // Share idle CPU slots between Max preparation and its dependent stage.
    bool stabilizeTemporalQuantizer=false; // Experimental ordered clip rate control; enable after source-quality validation.
    uint32_t keyframeInterval=120; // Frame-age limit; 0 disables it. Changes also enforce <=5 s; exact holds do not refresh.
    uint32_t similarTolerance=0,similarHoldFrames=10; // Opt-in max RGB source error 0..16; block refresh after 1..120 frames.
};
struct Progress {
    uint64_t frames=0,bytes=0;
    int64_t position=0,duration=0;
    double elapsed=0;
    uint32_t workers=0;
    uint32_t preparationWorkers=0; // Nonzero when Max prepares the following intra in parallel.
    bool sharedWorkers=false; // Stage ceilings overlap, but their shared CPU admission limit remains workers.
    GpuActivity gpu; // Completed compute work from the caller AND preparation workers; excludes native media decoding.
};
Progress convert(const Path& input,const Path& output,const ConvertOptions& options,std::stop_token stop,const std::function<void(const Progress&)>& progress={});
}
