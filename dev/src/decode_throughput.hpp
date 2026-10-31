#pragma once
#include "fajk/types.hpp"
#include <array>
#include <chrono>

namespace fajk {
// The owner serializes access. Count the union of active job intervals, so
// overlapping lanes are not added twice and a full prefetch queue adds no
// playback pacing time. The last 32 completions bound the displayed average.
class DecodeThroughput {
public:
    using Clock=std::chrono::steady_clock;
    struct Sample {double msPerFrame=-1;uint32_t frames=0;};
private:
    static constexpr uint32_t window=32;
    Clock::time_point started_{};
    Clock::duration busy_{};
    std::array<Clock::duration,window+1> completedAt_{};
    uint64_t completed_=0;
    uint32_t active_=0;
    Clock::duration busyAt(Clock::time_point now)const{return busy_+(active_?now-started_:Clock::duration{});}
public:
    void begin(Clock::time_point now){if(!active_++)started_=now;}
    void finish(Clock::time_point now,bool success=true){
        require(active_>0,"Decode throughput has no active job.");
        if(!--active_)busy_+=now-started_;
        if(success){++completed_;completedAt_[completed_%completedAt_.size()]=busyAt(now);}
    }
    Sample sample(Clock::time_point now)const{
        if(!completed_)return {};
        const auto frames=uint32_t(std::min<uint64_t>(window,completed_));
        const auto baseline=completed_>window?completedAt_[(completed_-window)%completedAt_.size()]:Clock::duration{};
        const auto ms=std::chrono::duration<double,std::milli>(busyAt(now)-baseline).count();
        return {ms>0?ms/frames:-1,frames};
    }
};
}
