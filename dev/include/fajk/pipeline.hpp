#pragma once
#include "types.hpp"
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
    uint32_t keyframeInterval=120; // 0: only content/size driven; <=1% changed pixels defer the age refresh.
    uint32_t similarTolerance=0,similarHoldFrames=10; // Opt-in max RGB source error 0..16; block refresh after 1..120 frames.
};
struct Progress {
    uint64_t frames=0,bytes=0;
    int64_t position=0,duration=0;
    double elapsed=0;
    uint32_t workers=0;
};
Progress convert(const Path& input,const Path& output,const ConvertOptions& options,std::stop_token stop,const std::function<void(const Progress&)>& progress={});
}
