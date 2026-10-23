#pragma once
#include "fajk/types.hpp"
namespace fajk {
struct FlatRegionLayout;
// GPU acceleration of the approved full-resolution reference. Never changes
// pixels on failure: callers may safely fall back to the CPU implementation.
bool reconstruct441Gpu(std::span<uint8_t> pixels, uint32_t width, uint32_t height,
                       float amount, float stronger, float trim, float aa=0, float contrast=0, bool protectText=false);
std::string reconstruction441GpuDevice();
void reconstruction441GpuDiagnostics(bool enabled);
std::string reconstruction441GpuProfile();
bool reconstruct441GpuScaled(std::span<const uint8_t> input,uint32_t iw,uint32_t ih,
                             std::span<uint8_t> output,uint32_t w,uint32_t h,float upscale,
                             const FlatRegionLayout* flat,float amount,float stronger,float trim,
                             float aa,float contrast,bool protectText);
}
