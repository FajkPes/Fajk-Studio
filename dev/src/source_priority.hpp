#pragma once
#include "fajk/types.hpp"
#include "fajk/hardware.hpp"
#include "gpu_compute.hpp"
#include <array>

namespace fajk {
// Encoder-only source importance. The existing adaptive quantizer records its
// chosen half/base/double step; this analysis adds no decoder metadata or work.
class SourcePriority {
  public:
    SourcePriority(const Frame& source, uint32_t threads, int mode, bool extendedFlat = true)
        : width_(source.width), height_(source.height), cell_(mode >= 29 ? 8u : 16u),
          cols_((width_ + cell_ - 1) / cell_), rows_((height_ + cell_ - 1) / cell_), mode_(mode) {
        source.validate();
        require(mode >= 1 && mode <= 37, "Invalid source priority mode.");
        Bytes classification(size_t(cols_) * rows_), nearFlat(extendedFlat ? size_t(cols_) * rows_ : 0);
        std::vector<GpuSourceStatistics> gpuStatistics;
        if(computeBackend()==ComputeBackend::Gpu){
            gpuStatistics.resize(size_t(cols_)*rows_);
            if(!gpuSourceStatistics(source,cell_,mode,gpuStatistics))gpuStatistics.clear();
        }
        parallelFor(rows_, threads, [&](uint32_t row) {
            for (uint32_t col = 0; col < cols_; ++col) {
                const uint32_t x0 = col * cell_, x1 = std::min(width_, x0 + cell_), y0 = row * cell_,
                               y1 = std::min(height_, y0 + cell_);
                const uint32_t count = (x1 - x0) * (y1 - y0);
                if (count < 16)
                    continue;
                double xx = 0, xy = 0, yy = 0;
                uint32_t quiet = 0, strong = 0;
                std::array<double, 3> sum{}, sumX{}, sumY{}, square{};
                bool opaque = true;
                const double cx = (x0 + x1 - 1) * .5, cy = (y0 + y1 - 1) * .5;
                // Closed-form sums of centered squared coordinates. Half-pixel
                // centers make these exact, including partial border cells.
                const double nx = x1 - x0, ny = y1 - y0;
                const double normX = count * (nx * nx - 1) / 12., normY = count * (ny * ny - 1) / 12.;
                if(!gpuStatistics.empty()){
                    const auto& s=gpuStatistics[size_t(row)*cols_+col];xx=s.xx4*.25;xy=s.xy4*.25;yy=s.yy4*.25;
                    quiet=s.quiet;strong=s.strong;opaque=s.opaque!=0;
                    for(int c=0;c<3;++c){sum[c]=s.sum[c];sumX[c]=s.sumX2[c]*.5;sumY[c]=s.sumY2[c]*.5;square[c]=s.square[c];}
                }else for (uint32_t y = y0; y < y1; ++y)
                    for (uint32_t x = x0; x < x1; ++x) {
                        const auto* p = source.pixels.data() + (size_t(y) * width_ + x) * 4;
                        const auto* left = source.pixels.data() + (size_t(y) * width_ + (x ? x - 1 : x)) * 4;
                        const auto* right =
                            source.pixels.data() + (size_t(y) * width_ + std::min(width_ - 1, x + 1)) * 4;
                        const auto* up = source.pixels.data() + (size_t(y ? y - 1 : y) * width_ + x) * 4;
                        const auto* down =
                            source.pixels.data() + (size_t(std::min(height_ - 1, y + 1)) * width_ + x) * 4;
                        opaque = opaque && p[3] == 255 && left[3] == 255 && right[3] == 255 && up[3] == 255 &&
                                 down[3] == 255;
                        const double ax = x - cx, ay = y - cy;
                        double gradient = 0;
                        for (unsigned c = 0; c < 3; ++c) {
                            const double dx = (int(right[c]) - int(left[c])) * .5,
                                         dy = (int(down[c]) - int(up[c])) * .5;
                            xx += dx * dx;
                            xy += dx * dy;
                            yy += dy * dy;
                            gradient += dx * dx + dy * dy;
                            sum[c] += p[c];
                            sumX[c] += p[c] * ax;
                            sumY[c] += p[c] * ay;
                            square[c] += double(p[c]) * p[c];
                        }
                        if (mode >= 3) {
                            quiet += gradient <= (mode >= 30 ? 16 : 4);
                            strong += gradient >= (mode >= 28 ? 144 : 1024);
                        }
                    }
                if (!opaque)
                    continue;
                const double energy = xx + yy,
                             coherence = energy > 0 ? std::sqrt((xx - yy) * (xx - yy) + 4 * xy * xy) / energy : 0;
                const double strength = std::sqrt(energy / count);
                // A low-error RGB plane includes constant color and legitimate
                // smooth shading. Random detail does not qualify as a flat area.
                double residual = 0;
                for (unsigned c = 0; c < 3; ++c) {
                    const double error = square[c] - sum[c] * sum[c] / count - (normX ? sumX[c] * sumX[c] / normX : 0) -
                                         (normY ? sumY[c] * sumY[c] / normY : 0);
                    residual += std::max(0., error) / count;
                }
                // A smooth strong ramp is a plane, not a discontinuity. Assigning
                // it the edge label would spend fine-band bits without a boundary.
                const bool plane = residual <= 12.;
                // A dark step can fit a low-error plane in absolute RGB units
                // yet still be a visible boundary. Require both directional
                // coherence and deviation from that plane relative to its
                // gradient; a true smooth ramp has essentially zero residual.
                const double brightestMean = *std::max_element(sum.begin(),sum.end()) / count;
                const bool subtleStep = mode >= 30 && brightestMean < 96 && strength >= 2.5 && coherence >= .90 &&
                                        residual >= 1.5 && residual >= .35 * strength * strength;
                // Tiny glyphs, curved icons and intersecting strokes can have a
                // weak global tensor despite clean high-contrast local geometry.
                // Sparse strong gradients on mostly quiet source colors protect
                // these graphics without promoting ordinary irregular texture.
                const bool expanded = mode == 19 || mode == 20 || mode >= 29;
                const bool graphic = mode >= 3 &&
                                     quiet * 10 >= count * (mode >= 30 ? 4
                                                            : expanded ? 5
                                                                       : 7) &&
                                     strong >= 2 &&
                                     strong * 10 <= count * (mode >= 30 ? 6
                                                             : expanded ? 5
                                                                        : 3);
                const double minimumCoherence = mode == 19 ? .45 : mode == 20 ? .6 : mode >= 30 ? .55 : .7;
                if (subtleStep || !plane && (strength >= 6 && coherence >= minimumCoherence || graphic))
                    classification[size_t(row) * cols_ + col] = 1;
                else if (plane)
                    classification[size_t(row) * cols_ + col] = 2;
                else if (mode >= 22 && strength >= 6 && residual >= 96)
                    classification[size_t(row) * cols_ + col] = 3;
                // A softly textured, similar-color wall can fail the strict plane
                // test without containing an object boundary. Keep a graded RGB
                // candidate score, but never promote protected geometry or strong
                // texture. This is reused for all slider values, no second scan.
                if (!extendedFlat)
                    continue;
                const auto state = classification[size_t(row) * cols_ + col];
                // Every recognized edge remains geometry, including low-
                // contrast boundaries. Only unclassified low-error texture
                // may become a near-flat candidate at higher tolerance.
                if (state == 0) {
                    const double score =
                        std::clamp((96. - residual) / 84., 0., 1.) * std::clamp((18. - strength) / 12., 0., 1.);
                    nearFlat[size_t(row) * cols_ + col] = uint8_t(score * 255 + .5);
                }
            }
        });
        // Prefix integrals make every coefficient-footprint lookup constant
        // time, including coarse bands spanning thousands of source pixels.
        const size_t stride = size_t(cols_) + 1;
        edge_.assign(stride * (rows_ + 1), 0);
        flat_.assign(stride * (rows_ + 1), 0);
        if (extendedFlat) {
            nearFlat_.assign(stride * (rows_ + 1), 0);
        }
        if (mode >= 22)
            texture_.assign(stride * (rows_ + 1), 0);
        for (uint32_t y = 0; y < rows_; ++y)
            for (uint32_t x = 0; x < cols_; ++x) {
                const auto state = classification[size_t(y) * cols_ + x];
                const auto at = (size_t(y) + 1) * stride + x + 1;
                edge_[at] = uint32_t(state == 1) + edge_[at - 1] + edge_[at - stride] - edge_[at - stride - 1];
                flat_[at] = uint32_t(state == 2) + flat_[at - 1] + flat_[at - stride] - flat_[at - stride - 1];
                if (extendedFlat)
                    nearFlat_[at] = uint32_t(nearFlat[size_t(y) * cols_ + x]) + nearFlat_[at - 1] + nearFlat_[at - stride] -
                                    nearFlat_[at - stride - 1];
                if (mode >= 22)
                    texture_[at] =
                        uint32_t(state == 3) + texture_[at - 1] + texture_[at - stride] - texture_[at - stride - 1];
            }
    }
    float bandRate(uint32_t originX, uint32_t originY, uint32_t footprintW, uint32_t footprintH) const {
        require(footprintW && footprintH, "Invalid source priority footprint.");
        if (originX >= width_ || originY >= height_)
            return 1;
        const double x0 = originX / double(cell_), y0 = originY / double(cell_),
                     x1 = std::min<uint64_t>(width_, uint64_t(originX) + footprintW) / double(cell_),
                     y1 = std::min<uint64_t>(height_, uint64_t(originY) + footprintH) / double(cell_);
        const double area = (x1 - x0) * (y1 - y0), edgeArea = rectangle(edge_, x0, y0, x1, y1),
                     flatArea = rectangle(flat_, x0, y0, x1, y1);
        // Coverage-weighted modes avoid promoting a whole group merely
        // because a narrow boundary touches it. Keep coarse mean and slope.
        if (mode_ >= 26 && mode_ <= 27) {
            const auto span = std::max(footprintW, footprintH);
            const double halo = span / double(2 * cell_);
            const double hx0 = std::max(0., x0 - halo), hy0 = std::max(0., y0 - halo),
                         hx1 = std::min(width_ / double(cell_), x1 + halo),
                         hy1 = std::min(height_ / double(cell_), y1 + halo);
            const double haloEdges = rectangle(edge_, hx0, hy0, hx1, hy1);
            if (mode_ == 27 && haloEdges > 0)
                return .75f;
            const double coverage = std::clamp(haloEdges / ((hx1 - hx0) * (hy1 - hy0)), 0., 1.);
            const double texture = span <= 64 ? rectangle(texture_, x0, y0, x1, y1) / area : 0.;
            return std::clamp(float(1 - .5 * std::sqrt(coverage) + texture * (1 - coverage)), .5f, 2.f);
        }
        if (mode_ >= 33) {
            const auto span = std::max(footprintW, footprintH);
            // Leave coarse mean/slope neutral; measure the actual edge share
            // rather than promoting every group intersected by one edge cell.
            if (span > 64) {
                if (mode_ <= 35)
                    return 1.f;
                const double halo = span / double(2 * cell_);
                return rectangle(edge_, std::max(0., x0 - halo), std::max(0., y0 - halo),
                                 std::min(width_ / double(cell_), x1 + halo),
                                 std::min(height_ / double(cell_), y1 + halo)) > 0
                           ? .25f
                           : 1.f;
            }
            const double halo = span / double((mode_ == 34 || mode_ == 37 ? 4 : 2) * cell_);
            const double hx0 = std::max(0., x0 - halo), hy0 = std::max(0., y0 - halo),
                         hx1 = std::min(width_ / double(cell_), x1 + halo),
                         hy1 = std::min(height_ / double(cell_), y1 + halo);
            const double edges = rectangle(edge_, hx0, hy0, hx1, hy1);
            if ((mode_ == 34 || mode_ == 37) && edges > 0)
                return .25f;
            const double coverage = std::clamp(edges / ((hx1 - hx0) * (hy1 - hy0)), 0., 1.);
            const double texture = rectangle(texture_, x0, y0, x1, y1) / area;
            const double protectedShare =
                mode_ == 35 || mode_ == 36 ? std::sqrt(std::sqrt(coverage)) : std::sqrt(coverage);
            return std::clamp(float(1 - .75 * protectedShare + texture * (1 - coverage)), .25f, 2.f);
        }
        if (mode_ >= 22) {
            const auto span = std::max(footprintW, footprintH);
            const double halo = span / double(2 * cell_);
            if (rectangle(edge_, std::max(0., x0 - halo), std::max(0., y0 - halo),
                          std::min(width_ / double(cell_), x1 + halo),
                          std::min(height_ / double(cell_), y1 + halo)) > 0)
                return mode_ == 31 ? .25f : .5f;
            return span <= 64 ? std::clamp(float(1 + (mode_ == 32
                                                          ? .25f
                                                          : std::array<float, 4>{1.f, 3.f, -.5f,
                                                                                 -.75f}[mode_ >= 28 ? 0 : mode_ - 22]) *
                                                         rectangle(texture_, x0, y0, x1, y1) / area),
                                           .25f, 4.f)
                              : 1.f;
        }
        if (mode_ >= 19) {
            const auto span = std::max(footprintW, footprintH);
            const double halo = span / double(2 * cell_);
            if (rectangle(edge_, std::max(0., x0 - halo), std::max(0., y0 - halo),
                          std::min(width_ / double(cell_), x1 + halo),
                          std::min(height_ / double(cell_), y1 + halo)) > 0)
                return mode_ == 19 ? .5f : .25f;
            return mode_ == 20 && span <= 64 ? std::clamp(float(1 + flatArea / area), 1.f, 2.f) : 1.f;
        }
        if (mode_ >= 13) {
            const auto variant = mode_ - 13;
            const auto span = std::max(footprintW, footprintH);
            if (variant == 4 && span > 64)
                return 1.f;
            const double halo = span / double(2 * cell_);
            const bool protectedEdge = rectangle(edge_, std::max(0., x0 - halo), std::max(0., y0 - halo),
                                                 std::min(width_ / double(cell_), x1 + halo),
                                                 std::min(height_ / double(cell_), y1 + halo)) > 0;
            const float edgeFactor = variant == 3 ? .75f : .5f;
            if (protectedEdge)
                return edgeFactor;
            const float flatFactor = span <= 64 ? std::array<float, 6>{1.f, 2.f, 4.f, 8.f, 8.f, 8.f}[variant] : 1.f;
            return std::clamp(float(1 + (flatFactor - 1) * flatArea / area), 1.f, 8.f);
        }
        if (mode_ >= 10) {
            // Fine wavelet coefficients influence pixels beyond their center.
            // Protect a source boundary across the extended basis footprint.
            const double halo = std::max(footprintW, footprintH) / double(2 * cell_);
            if (rectangle(edge_, std::max(0., x0 - halo), std::max(0., y0 - halo),
                          std::min(width_ / double(cell_), x1 + halo),
                          std::min(height_ / double(cell_), y1 + halo)) > 0)
                return .5f;
        }
        if (mode_ >= 7) {
            const float flatFactor =
                std::max(footprintW, footprintH) <= 64 ? std::array<float, 3>{8.f, 32.f, 128.f}[(mode_ - 7) % 3] : 1.f;
            return std::clamp(float(1 - .5 * edgeArea / area + (flatFactor - 1) * flatArea / area), .5f, 128.f);
        }
        if (mode_ >= 5) {
            const double flatFactor = std::max(footprintW, footprintH) <= 64 ? (mode_ == 5 ? 2. : 4.) : 1.;
            const double edgeFactor = mode_ == 5 ? 1. : .75;
            return std::clamp(float(1 + (edgeFactor - 1) * edgeArea / area + (flatFactor - 1) * flatArea / area), .75f,
                              4.f);
        }
        const bool mild = mode_ % 2 != 0;
        const float edgeFactor = mild ? .5f : .25f;
        // Reserve the plane's mean and slope in coarse bands. Only fine-band
        // quantizers see a higher rate penalty for a source-explained plane.
        const float flatFactor = std::max(footprintW, footprintH) <= 64 ? (mild ? 2.f : 4.f) : 1.f;
        const double average = 1 + (edgeFactor - 1) * edgeArea / area + (flatFactor - 1) * flatArea / area;
        // Even a narrow coherent source edge wins over surrounding flat cells.
        return std::clamp(float(edgeArea > 0 ? std::min<double>(average, edgeFactor) : average), edgeFactor,
                          mild ? 2.f : 4.f);
    }
    bool hasDirectEdge(uint32_t originX, uint32_t originY, uint32_t footprintW, uint32_t footprintH) const {
        // Use the same recognized geometry as bandRate, not a second, stronger
        // intensity threshold that would discard weak object boundaries.
        if (!footprintW || !footprintH || originX >= width_ || originY >= height_)
            return false;
        const double x0 = originX / double(cell_), y0 = originY / double(cell_),
                     x1 = std::min<uint64_t>(width_, uint64_t(originX) + footprintW) / double(cell_),
                     y1 = std::min<uint64_t>(height_, uint64_t(originY) + footprintH) / double(cell_);
        return rectangle(edge_, x0, y0, x1, y1) > 0;
    }
    bool hasEdgeWithMargin(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t margin) const {
        const uint32_t x0=x>margin?x-margin:0,y0=y>margin?y-margin:0;
        return hasDirectEdge(x0,y0,w+x-x0+margin,h+y-y0+margin);
    }
    float flatCoverage(uint32_t originX, uint32_t originY, uint32_t footprintW, uint32_t footprintH,
                       uint32_t tolerance = 50) const {
        if (!footprintW || !footprintH || originX >= width_ || originY >= height_)
            return 0;
        const double x0 = originX / double(cell_), y0 = originY / double(cell_),
                     x1 = std::min<uint64_t>(width_, uint64_t(originX) + footprintW) / double(cell_),
                     y1 = std::min<uint64_t>(height_, uint64_t(originY) + footprintH) / double(cell_);
        require(tolerance <= 100, "Invalid flat tolerance.");
        require(tolerance <= 50 || !nearFlat_.empty(), "Extended flat tolerance was not requested.");
        double eligible = rectangle(flat_, x0, y0, x1, y1);
        if (tolerance > 50)
            eligible += (tolerance - 50) / 50. * rectangle(nearFlat_, x0, y0, x1, y1) / 255.;
        return std::clamp(float(eligible / ((x1 - x0) * (y1 - y0))), 0.f, 1.f);
    }

  private:
    uint32_t width_, height_, cell_, cols_, rows_;
    int mode_;
    std::vector<uint32_t> edge_, flat_, texture_, nearFlat_;
    double integral(const std::vector<uint32_t>& prefix, double x, double y) const {
        const uint32_t ix = uint32_t(x), iy = uint32_t(y), nx = std::min(cols_, ix + 1), ny = std::min(rows_, iy + 1);
        const double fx = x - ix, fy = y - iy;
        const size_t stride = size_t(cols_) + 1;
        return (1 - fy) * ((1 - fx) * prefix[size_t(iy) * stride + ix] + fx * prefix[size_t(iy) * stride + nx]) +
               fy * ((1 - fx) * prefix[size_t(ny) * stride + ix] + fx * prefix[size_t(ny) * stride + nx]);
    }
    double rectangle(const std::vector<uint32_t>& prefix, double x0, double y0, double x1, double y1) const {
        return std::max(0., integral(prefix, x1, y1) - integral(prefix, x0, y1) - integral(prefix, x1, y0) +
                                integral(prefix, x0, y0));
    }
};
} // namespace fajk
