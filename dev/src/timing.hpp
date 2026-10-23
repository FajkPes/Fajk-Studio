#pragma once
#include <chrono>
#include <vector>
namespace fajk {
struct TimingEvent {const char* stage;double ms;};
// Caller-thread wall times include waiting for workers. Nested pack stages
// overlap rate_search; never add both to calculate the total.
// encode.detail.* intervals are also nested in their enclosing legacy stage.
inline thread_local std::vector<TimingEvent>* timingEvents=nullptr;
struct TimingCapture {
    std::vector<TimingEvent>* previous;
    explicit TimingCapture(std::vector<TimingEvent>* events):previous(timingEvents){timingEvents=events;}
    ~TimingCapture(){timingEvents=previous;}
};
struct TimingTimeline {
    std::chrono::steady_clock::time_point start=timingEvents?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    void mark(const char* stage){if(!timingEvents)return;auto now=std::chrono::steady_clock::now();timingEvents->push_back({stage,std::chrono::duration<double,std::milli>(now-start).count()});start=now;}
};
}
