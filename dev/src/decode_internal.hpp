#pragma once
#include "packet_view.hpp"
#include "text_guard.hpp"
namespace fajk {
// Native-size CPU cleanup and upscale share the decoded working image.
// Flat metadata remains owned here until reconstruction finishes.
struct DeferredUpscale {
    bool active=false,skipBaseCleanup=false;
    uint32_t budget=0,surfaces=0,contours=0;
    Bytes flatMetadata;
    Bytes* recycle=nullptr;
    bool protectText=false;
    DecodedTextProtection protectedText;
};
Frame decodeFrameDeferred(PacketView,uint32_t,uint32_t,uint32_t,DeferredUpscale&,std::span<uint8_t> = {});
// Only nested decoder layers: workers must come from the outer safeWorkers.
Frame decodeNestedFrame(PacketView,uint32_t,uint32_t,uint32_t,std::span<uint8_t>,DeferredUpscale*,uint32_t = 100,uint32_t = 0);
Frame decodeFrameWithSettings(PacketView, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t = 0, bool = false,ComputeBackend = ComputeBackend::Cpu);
// Contiguous caller-owned RGBA; no additional output image allocation/copy.
void decodeFrameToBuffer(PacketView, uint32_t, uint32_t, uint32_t, uint32_t, std::span<uint8_t>, uint32_t = 0,
                         bool = false, ComputeBackend = ComputeBackend::Cpu);
} // namespace fajk
