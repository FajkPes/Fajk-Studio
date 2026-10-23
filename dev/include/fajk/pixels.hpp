#pragma once
#include "types.hpp"
namespace fajk {
// Converts straight RGBA to the premultiplied BGRA required by WinUI.
// Exact in-place conversion is supported; otherwise the buffers must not overlap.
void displayPixels(std::span<const uint8_t> rgba, std::span<uint8_t> bgra);
}
