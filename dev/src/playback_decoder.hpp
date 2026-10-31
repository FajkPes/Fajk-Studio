#pragma once
#include "fajk/types.hpp"
#include <memory>
#include <optional>

namespace fajk {
struct DecodeRequest {
    Path path,source;
    uint64_t frame=0,generation=0,contentRevision=0;
    int64_t pts=0;
    ComputeBackend backend=ComputeBackend::Cpu;
    uint32_t threads=0;
    uint64_t memoryMiB=0,last=0;
    uint32_t step=1,fpsNum=30,fpsDen=1;
    int mode=1;
    bool continuous=false;
    uint32_t deblocking=100,sharpening=50;
    uint32_t decodeLanes=0; // 0 automatic, 1 serial, 2 parallel diagnostic override; RAM/CPU limits still apply.
};
struct DecodeResult {
    uint64_t frame=0,generation=0;
    double ms=0,decodeMs=-1;
    double throughputMs=-1;
    uint32_t throughputFrames=0;
    uint32_t filter=100,sharpening=50;
    uint32_t lanes=1;
    std::shared_ptr<Frame> image,reference;
    std::wstring error;
};
// Serialized source/sequence ownership, bounded prefetch and generation
// cancellation. Contains no XAML objects and never accesses the UI thread.
class PlaybackDecoder {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit PlaybackDecoder(Path trace={});
    ~PlaybackDecoder();
    PlaybackDecoder(const PlaybackDecoder&)=delete;
    PlaybackDecoder& operator=(const PlaybackDecoder&)=delete;
    void invalidate(uint64_t generation);
    void submit(DecodeRequest);
    std::optional<DecodeResult> takeDue(uint64_t generation,bool playing,int64_t now,uint64_t* skipped=nullptr);
    void shutdown();
};
}
