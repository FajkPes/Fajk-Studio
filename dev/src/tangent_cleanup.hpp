#pragma once
#include "fajk/types.hpp"
namespace fajk {
bool hasAvx2();
// Decoded-pixel-only experimental cleanup. The source remains immutable while
// rendering, including across worker boundaries. Old packet paths do not call it.
void tangentCleanup(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float strength=1.f,float radius=5.f,float range=20.f);
void compactTangentCleanup(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float strength=1.f,uint32_t scale=4,float trim=-1,float stronger=0);
void contourAntialias(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float strength=1.f,float contrast=0.f);
// Detect compact neutral strokes from the unfiltered decode and restore those
// regions after cleanup. Neither source pixels nor application-specific ROIs.
void preserveDecodedText(std::span<uint8_t> candidate,std::span<const uint8_t> decoded,uint32_t width,uint32_t height,uint32_t workers);
void boundedContourContrast(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float strength=.5f);
}
