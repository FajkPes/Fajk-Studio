#pragma once
#include "fajk/hardware.hpp"
#include <immintrin.h>

namespace fajk {
// Precompensate the known bilinear decoder filter before quantization. Limit
// overshoot so saturated contours cannot produce unbounded color coefficients.
// This changes only encoder samples; released packet interpretation is intact.
inline void precompensateChroma(std::vector<float>& plane, int stride, int height,
                               uint32_t threads) {
    thread_local std::vector<float> cached;
    std::vector<float> owned;
    auto& original = plane.size() <= 8 * 1024 * 1024 ? cached : owned;
    original.assign(plane.begin(), plane.end());
    parallelFor(uint32_t((height + 15) / 16), threads, [&](uint32_t job) {
        for (int y = int(job) * 16; y < std::min(height, int(job + 1) * 16); ++y) {
            const auto* row = original.data() + size_t(y) * stride;
            const auto* up = original.data() + size_t(std::max(0, y - 1)) * stride;
            const auto* down = original.data() + size_t(std::min(height - 1, y + 1)) * stride;
            auto* dst = plane.data() + size_t(y) * stride;
            auto scalar = [&](int x) {
                const float average = ((up[x] + down[x]) +
                    (row[std::max(0, x - 1)] + row[std::min(stride - 1, x + 1)])) * .25f;
                dst[x] = row[x] + std::clamp((row[x] - average) * .5f, -16.f, 16.f);
            };
            scalar(0);
            int x = 1;
            for (; x + 4 < stride; x += 4) {
                const auto v = _mm_loadu_ps(row + x);
                const auto average = _mm_mul_ps(_mm_add_ps(
                    _mm_add_ps(_mm_loadu_ps(up + x), _mm_loadu_ps(down + x)),
                    _mm_add_ps(_mm_loadu_ps(row + x - 1), _mm_loadu_ps(row + x + 1))),
                    _mm_set1_ps(.25f));
                const auto delta = _mm_mul_ps(_mm_sub_ps(v, average), _mm_set1_ps(.5f));
                _mm_storeu_ps(dst + x, _mm_add_ps(v,
                    _mm_min_ps(_mm_set1_ps(16), _mm_max_ps(_mm_set1_ps(-16), delta))));
            }
            for (; x < stride; ++x) scalar(x);
        }
    });
}
}
