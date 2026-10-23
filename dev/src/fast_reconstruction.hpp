#pragma once
#include "fajk/types.hpp"
namespace fajk {
struct FlatRegionLayout;
struct DecodedTextProtection;
inline constexpr uint32_t recursiveCleanupTile=96;
void recursiveContourCleanup(std::span<uint8_t>,uint32_t,uint32_t,uint32_t,float,float,std::span<const uint8_t> = {},float curvature=0,std::span<const uint8_t> skipTiles={},bool fusedArithmetic=true);
Frame fastReconstruct441(Frame& native,uint32_t width,uint32_t height,uint32_t workers,
                         uint32_t budget,const Reconstruction441&,std::span<uint8_t> destination={},const FlatRegionLayout* flat=nullptr,const DecodedTextProtection* protectedText=nullptr);
}
