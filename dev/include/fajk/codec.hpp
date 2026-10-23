#pragma once
#include "types.hpp"
#include <memory>

namespace fajk {
// A codec instance is owned by a single caller; internal jobs share one pool.
// New backends implement this interface without touching the container or UI.
class FrameCodec {
public:
    virtual ~FrameCodec() = default;
    virtual Packet encode(const Frame& frame, const EncodeOptions& options) = 0;
    virtual Frame decode(const Packet& packet, uint32_t width, uint32_t height) = 0;
    virtual void setThreads(uint32_t threads) = 0;
    virtual void setComputeBackend(ComputeBackend backend) = 0;
    virtual ComputeBackend backend() const = 0;
    // Playback/export setting; does not alter the packet. 0 skips filtering.
    virtual void setDeblocking(uint32_t percent) = 0;
    // Final display sharpening after all reconstruction; 0 skips the work.
    virtual void setSharpening(uint32_t percent) = 0;
};
std::unique_ptr<FrameCodec> makeCodec(CodecId id);
Frame readImage(const Path& path);
void writePng(const Path& path, const Frame& frame);
}
