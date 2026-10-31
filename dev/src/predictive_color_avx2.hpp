#pragma once
#include <array>
#include <cstdint>
namespace fajk {
// Processes complete groups of eight pixels. The caller handles the tail and
// checks hasAvx2(). Float operation order and half-away rounding are canonical.
uint32_t predictiveColorRowAvx2(uint8_t* rgba,uint32_t width,uint32_t chromaWidth,
    const float* luma,const float* coTop,const float* coBottom,
    const float* cgTop,const float* cgBottom,const uint16_t* patterns,
    const std::array<uint8_t,64>* blendTable,uint32_t phase,float fy);
}
