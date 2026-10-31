#pragma once
#include "fajk/types.hpp"
#include "fajk/compute.hpp"
#include <atomic>
#include <memory>
#include <optional>

namespace fajk {
struct SequenceDecodeSettings {
    Path path;
    uint64_t generation=0,contentRevision=0,memoryMiB=0;
    uint32_t width=0,height=0,threads=0,deblocking=100,sharpening=0;
    ComputeBackend backend=ComputeBackend::Cpu;
    bool borrowCpuWorkers=false;
    bool includeLast=false; // Playback also presents the final frame when the stride does not divide the range.
    const std::atomic<uint64_t>* epoch=nullptr; // Optional owner token; must outlive this queue's workers.
};
struct SequenceDecodeResult {
    uint64_t frame=0,generation=0;
    std::shared_ptr<Frame> image;
    double decodeMs=0;
    double throughputMs=-1;
    uint32_t throughputFrames=0;
    GpuActivity gpu;
    std::string error;
};
// Ordered delivery from a bounded scheduling window. Each lane owns its reader,
// codec, reference state and frame pool. Chunks keep nearby dependencies hot.
// start/cancel/take may run on different threads; no UI or media source lives here.
class SequenceDecodeAhead {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit SequenceDecodeAhead(uint32_t lanes);
    ~SequenceDecodeAhead();
    SequenceDecodeAhead(const SequenceDecodeAhead&)=delete;
    SequenceDecodeAhead& operator=(const SequenceDecodeAhead&)=delete;
    void start(SequenceDecodeSettings,uint64_t first,uint64_t last,uint32_t step=1,uint32_t window=5,uint32_t chunk=2);
    void cancel();
    std::optional<SequenceDecodeResult> take(uint64_t frame,uint64_t generation,const std::atomic<uint64_t>* epoch=nullptr);
};
}
