#pragma once
#include "wavelet.hpp"
#include "fajk/hardware.hpp"
#include <immintrin.h>
namespace fajk {
// Fit the interpolated field to the detail removed by 4:2:0. Integrate
// each bilinear patch once, then solve the same two bounded Jacobi updates.
inline void refineInterpolatedChroma(const WaveletSourceColor& source, uint32_t tile, uint32_t workers,
                                     std::span<uint8_t> map) {
    const auto width = source.width, height = source.height;
    pixelBytes(width, height);
    require(tile == 64, "Invalid interpolated chroma tile.");
    const uint32_t cols = (width + tile - 1) / tile, rows = (height + tile - 1) / tile, stride = (width + 63) & ~63u;
    require(map.size() == size_t(cols) * rows * 2, "Invalid interpolated chroma map.");
    for (const auto& plane : source.planes)
        require(plane.size() >= size_t(stride) * height, "Invalid interpolated chroma source.");
    const float center = (tile - 1) * .5f;
    // The four patch corners may clamp to the same node at image borders.
    // Accumulate those contributions before computing the diagonal.
    std::vector<ColorModelPatch> ownedPatches;
    if(source.fittedPatches.empty())ownedPatches.resize(size_t(cols+1)*(rows+1));
    const auto& patches=source.fittedPatches.empty()?ownedPatches:source.fittedPatches;
    require(patches.size()==size_t(cols+1)*(rows+1),"Invalid shared color patch statistics.");
    if(source.fittedPatches.empty())parallelFor(rows + 1, workers, [&](uint32_t py) {
        for (uint32_t px = 0; px <= cols; ++px) {
            auto& patch = ownedPatches[size_t(py) * (cols + 1) + px];
            // Independent node accumulators share a SIMD lane without changing
            // sample order or double-precision arithmetic in any equation.
            __m128d cov[2][2]{}, matrix[4][2]{}, samples[2]{};
            const int ix = int(px) - 1, iy = int(py) - 1;
            const uint32_t x0 = uint32_t(std::max(0, int(px * tile) - int(tile / 2))),
                           y0 = uint32_t(std::max(0, int(py * tile) - int(tile / 2)));
            const uint32_t x1 = std::min(width, px * tile + tile / 2), y1 = std::min(height, py * tile + tile / 2);
            for (uint32_t y = y0; y < y1; y += 2) {
                const float fy = (y + .5f - center) / tile - iy;
                for (uint32_t x = x0; x < x1; x += 2) {
                    const float fx = (x + .5f - center) / tile - ix;
                    const double weights[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                    double sum[3]{}, yy = 0, yc[2]{};
                    for (int j = 0; j < 2; ++j)
                        for (int i = 0; i < 2; ++i) {
                            const size_t at = size_t(std::min(height - 1, y + j)) * stride + std::min(width - 1, x + i);
                            const double l = source.planes[0][at];
                            sum[0] += l;
                            yy += l * l;
                            for (int c = 0; c < 2; ++c) {
                                const double color = source.planes[c + 1][at];
                                sum[c + 1] += color;
                                yc[c] += l * color;
                            }
                        }
                    const double variance = std::max(0., yy - sum[0] * sum[0] * .25);
                    const double covariance[2] = {yc[0] - sum[0] * sum[1] * .25, yc[1] - sum[0] * sum[2] * .25};
                    const __m128d w[2]{_mm_loadu_pd(weights),_mm_loadu_pd(weights+2)};
                    for(int half=0;half<2;++half){
                        for(int c=0;c<2;++c)
                            cov[c][half]=_mm_add_pd(cov[c][half],_mm_mul_pd(w[half],_mm_set1_pd(covariance[c])));
                        for(int j=0;j<4;++j)
                            matrix[j][half]=_mm_add_pd(matrix[j][half],_mm_mul_pd(_mm_mul_pd(w[half],_mm_set1_pd(weights[j])),_mm_set1_pd(variance)));
                        samples[half]=_mm_add_pd(samples[half],w[half]);
                    }
                }
            }
            alignas(16) double values[4];
            for(int c=0;c<2;++c){
                _mm_store_pd(values,cov[c][0]);_mm_store_pd(values+2,cov[c][1]);
                for(int i=0;i<4;++i)patch.covariance[i][c]=values[i];
            }
            for(int j=0;j<4;++j){
                _mm_store_pd(values,matrix[j][0]);_mm_store_pd(values+2,matrix[j][1]);
                for(int i=0;i<=j;++i)patch.matrix[i][j]=values[i];
            }
            _mm_storeu_pd(patch.samples,samples[0]);_mm_storeu_pd(patch.samples+2,samples[1]);
        }
    });
    // A node couples only to its eight neighbors. Gathering disjoint patch
    // statistics avoids atomics and repeated reads of overlapping source areas.
    struct Equation {
        double covariance[2]{}, matrix[9]{}, samples{};
    };
    std::vector<Equation> equations(size_t(cols) * rows);
    for (uint32_t ty = 0; ty < rows; ++ty)
        for (uint32_t tx = 0; tx < cols; ++tx) {
            auto& eq = equations[size_t(ty) * cols + tx];
            for (uint32_t py = ty; py <= ty + 1; ++py)
                for (uint32_t px = tx; px <= tx + 1; ++px) {
                    const auto& patch = patches[size_t(py) * (cols + 1) + px];
                    int nx[4], ny[4];
                    for (int k = 0; k < 4; ++k) {
                        nx[k] = std::clamp(int(px) - 1 + k % 2, 0, int(cols) - 1);
                        ny[k] = std::clamp(int(py) - 1 + k / 2, 0, int(rows) - 1);
                    }
                    for (int i = 0; i < 4; ++i)
                        if (nx[i] == int(tx) && ny[i] == int(ty)) {
                            for (int c = 0; c < 2; ++c)
                                eq.covariance[c] += patch.covariance[i][c];
                            for (int j = 0; j < 4; ++j)
                                eq.matrix[(ny[j] - int(ty) + 1) * 3 + nx[j] - int(tx) + 1] +=
                                    patch.matrix[std::min(i, j)][std::max(i, j)];
                            eq.samples += patch.samples[i];
                        }
                }
        }
    // Snapshot each iteration: updating in place would make the result depend
    // on traversal order. Quantize to the existing signed 1/16 slope format.
    for (int iteration = 0; iteration < 2; ++iteration) {
        const Bytes previous(map.begin(), map.end());
        for (uint32_t ty = 0; ty < rows; ++ty)
            for (uint32_t tx = 0; tx < cols; ++tx) {
                const size_t node = size_t(ty) * cols + tx;
                const auto& eq = equations[node];
                for (int c = 0; c < 2; ++c) {
                    double gradient = eq.covariance[c];
                    for (int j = -1; j <= 1; ++j)
                        for (int i = -1; i <= 1; ++i) {
                            const size_t at = size_t(std::clamp(int(ty) + j, 0, int(rows) - 1)) * cols +
                                              std::clamp(int(tx) + i, 0, int(cols) - 1);
                            gradient -= eq.matrix[(j + 1) * 3 + i + 1] * int8_t(previous[at * 2 + c]) / 16.;
                        }
                    const double before = int8_t(previous[node * 2 + c]) / 16.;
                    const double delta = std::clamp(gradient / (3 * eq.matrix[4] + eq.samples + 1), -.25, .25);
                    map[node * 2 + c] = uint8_t(int8_t(std::clamp(int(std::lround((before + delta) * 16)), -24, 24)));
                }
            }
    }
}

} // namespace fajk
