#pragma once
#include "fajk/types.hpp"
namespace fajk {
// Block motion (32x32), signed quarter-pixel vectors and optional affine
// gradients. Processes full groups of eight, returning the scalar tail start.
// Caller validates sizes/ranges and checks hasAvx2(). Source/output must differ.
uint32_t motionWarpRowAvx2(const uint8_t* source,uint8_t* output,uint32_t width,uint32_t height,uint32_t y,
    std::span<const uint32_t> vectors,std::span<const uint32_t> gradients,
    std::span<const uint8_t> modes,const std::array<int,3>& bias);
}
