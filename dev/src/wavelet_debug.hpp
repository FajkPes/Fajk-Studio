#pragma once
#include "fajk/types.hpp"
#include <array>

namespace fajk {
// Instantiated only by the explicit inspector. Production decode compiles out
// every capture call, including the per-pixel component visualization.
struct WaveletDebugCapture {
    // 2=Y, 3/4=reconstructed chroma, 5/6=decoded residual, 7..9=coefficients.
    std::array<int, 2> views{};
    std::array<Frame, 2> images;
    float gain = 1;
    void pixel(size_t slot, uint32_t x, uint32_t y, float value, bool luma = false) {
        const auto byte = uint8_t(std::clamp(std::lround(128 + value * (luma ? 1.f : gain)), 0l, 255l));
        auto* p = images[slot].pixels.data() + (size_t(y) * images[slot].width + x) * 4;
        p[0] = p[1] = p[2] = byte;
        p[3] = 255;
    }
    void plane(size_t slot, const std::vector<float>& values, uint32_t stride, uint32_t width, uint32_t height,
               bool coefficient) {
        auto& image = images[slot];
        image = {width, height, 0, 0, Bytes(pixelBytes(width, height))};
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const float value = values[size_t(y) * stride + x];
                pixel(slot, x, y, coefficient ? 20.f * std::asinh(value / 4.f) : value, views[slot] == 2);
            }
    }
};
} // namespace fajk
