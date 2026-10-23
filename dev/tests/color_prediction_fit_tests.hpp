#pragma once
#include "../src/color_prediction_fit.hpp"
#include <random>

// Independent direct gradient oracle: visit pixels for every update, without
// patch matrices, neighbor equations, or the production partition/gather path.
inline fajk::Bytes directChromaFit(const fajk::WaveletSourceColor& source, fajk::Bytes map) {
    const auto w = source.width, h = source.height, stride = (w + 63) & ~63u;
    const uint32_t cols = (w + 63) / 64, rows = (h + 63) / 64;
    for (int iteration = 0; iteration < 2; ++iteration) {
        std::vector<double> gradient(map.size()), diagonal(map.size() / 2), samples(map.size() / 2);
        for (uint32_t y = 0; y < h; y += 2)
            for (uint32_t x = 0; x < w; x += 2) {
                const float gx = (x - 31.f) / 64, gy = (y - 31.f) / 64;
                const int ix = int(std::floor(gx)), iy = int(std::floor(gy));
                const float fx = gx - ix, fy = gy - iy;
                std::vector<double> weights(map.size() / 2);
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i)
                        weights[size_t(std::clamp(iy + j, 0, int(rows) - 1)) * cols +
                                std::clamp(ix + i, 0, int(cols) - 1)] += (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                double mean[3]{};
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i)
                        for (int c = 0; c < 3; ++c)
                            mean[c] +=
                                source.planes[c][size_t(std::min(h - 1, y + j)) * stride + std::min(w - 1, x + i)] *
                                .25;
                double variance = 0, covariance[2]{}, slope[2]{};
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i) {
                        const size_t at = size_t(std::min(h - 1, y + j)) * stride + std::min(w - 1, x + i);
                        const double dy = source.planes[0][at] - mean[0];
                        variance += dy * dy;
                        for (int c = 0; c < 2; ++c)
                            covariance[c] += dy * (source.planes[c + 1][at] - mean[c + 1]);
                    }
                for (size_t n = 0; n < weights.size(); ++n)
                    for (int c = 0; c < 2; ++c)
                        slope[c] += weights[n] * int8_t(map[n * 2 + c]) / 16.;
                for (size_t n = 0; n < weights.size(); ++n) {
                    for (int c = 0; c < 2; ++c)
                        gradient[n * 2 + c] += weights[n] * (covariance[c] - slope[c] * variance);
                    diagonal[n] += weights[n] * weights[n] * variance;
                    samples[n] += weights[n];
                }
            }
        for (size_t n = 0; n < diagonal.size(); ++n)
            for (int c = 0; c < 2; ++c) {
                const auto at = n * 2 + c;
                const double delta = std::clamp(gradient[at] / (3 * diagonal[n] + samples[n] + 1), -.25, .25);
                map[at] = uint8_t(int8_t(std::clamp(int(std::lround(int8_t(map[at]) + delta * 16)), -24, 24)));
            }
    }
    return map;
}

template <class Check> void testInterpolatedChroma(Check check) {
    std::mt19937 rng(460);
    for (auto [w, h] :
         {std::pair{1u, 1u}, {1u, 65u}, {65u, 1u}, {64u, 64u}, {65u, 67u}, {127u, 129u}, {129u, 127u}, {256u, 193u}}) {
        fajk::WaveletSourceColor source;
        source.width = w;
        source.height = h;
        const auto stride = (w + 63) & ~63u;
        for (auto& p : source.planes)
            p.resize(size_t(stride) * h, 9999.f);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t at = size_t(y) * stride + x;
                const float l = float(rng() % 1024) * .125f;
                source.planes[0][at] = l;
                // Correlation changes across cell boundaries and includes a dark edge.
                source.planes[1][at] = (x > w / 2 ? .75f : -.5f) * l + float(rng() % 64) * .125f;
                source.planes[2][at] = (y > h / 2 ? -.25f : .5f) * l + float(rng() % 64) * .125f;
            }
        fajk::Bytes initial(size_t((w + 63) / 64) * ((h + 63) / 64) * 2);
        for (auto& v : initial)
            v = uint8_t(int8_t(int(rng() % 49) - 24));
        const auto expected = directChromaFit(source, initial);
        for (uint32_t threads : {1u, 3u}) {
            auto map = initial;
            fajk::refineInterpolatedChroma(source, 64, threads, map);
            check(map == expected);
        }
        // Constant luminance contains no usable color predictor, even if color
        // varies; do not invent a correction or let padding contaminate it.
        std::fill(source.planes[0].begin(), source.planes[0].end(), 17.f);
        auto unchanged = initial;
        fajk::refineInterpolatedChroma(source, 64, 3, unchanged);
        check(unchanged == initial);
    }
}
