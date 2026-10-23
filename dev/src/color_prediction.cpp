#include "color_prediction.hpp"
#include "color_prediction_fit.hpp"
#include "perceptual_color.hpp"
#include "timing.hpp"
#include "wavelet.hpp"
#include "codec_internal.hpp"
#include "resample.hpp"
#include "fajk/hardware.hpp"
#include "fajk/entropy.hpp"
namespace fajk {
namespace {
struct ColorModel {
    Bytes map;
    std::vector<float> means;
};
// Fit the detail lost by 4:2:0 while this converted cell is still in cache.
// The broad covariance retains stable shading; the detail fit supplies an
// equally weighted estimate at color edges. High-density 32px cells keep the
// existing predictor, where this correction caused a regression on faces.
void refineDetailCell(const Frame& f,uint32_t tile,uint32_t sourceStride,uint32_t cell,
                      ColorModel& model,const WaveletSourceColor& source){
    const uint32_t modelColumns=(f.width+tile-1)/tile;
    double variance=0,covariance[2]{};uint32_t samples=0;
    const uint32_t x0=(cell%modelColumns)*tile,y0=(cell/modelColumns)*tile;
    for(uint32_t y=y0;y<std::min(f.height,y0+tile);y+=2)
        for(uint32_t x=x0;x<std::min(f.width,x0+tile);x+=2){
            const size_t at[4]={size_t(y)*sourceStride+x,size_t(y)*sourceStride+std::min(f.width-1,x+1),size_t(std::min(f.height-1,y+1))*sourceStride+x,size_t(std::min(f.height-1,y+1))*sourceStride+std::min(f.width-1,x+1)};
            double mean[3]{};for(int c=0;c<3;++c)for(auto i:at)mean[c]+=source.planes[c][i]*.25;
            for(auto i:at){const double dy=source.planes[0][i]-mean[0];variance+=dy*dy;for(int c=0;c<2;++c)covariance[c]+=dy*(source.planes[c+1][i]-mean[c+1]);}
            samples+=4;
        }
    for(int c=0;c<2;++c){const int q=std::clamp(int(std::lround(covariance[c]/(variance+samples*.25)*16)),-24,24);const double a=q/16.;const int detail=2*a*covariance[c]-a*a*variance>samples*.25?q:0;
        model.map[cell*2+c]=uint8_t(int8_t(std::lround((int8_t(model.map[cell*2+c])+detail)*.5)));}
}
// Double tree reductions differ only at the last arithmetic bits. Revisit
// cells near a discrete slope/gain threshold in the original CPU sample order.
bool colorFitBorder(const GpuColorStatistics& s,uint32_t tile){
    const double count=s.count,variance=std::max(0.,s.square-s.sum*s.sum/count);
    auto border=[](double covariance,double variance,double regularizer){
        const double value=covariance/(variance+regularizer)*16;
        if(std::abs((value-std::floor(value))-.5)<1e-6)return true;
        const double q=std::clamp(std::round(value),-24.,24.),a=q/16.;
        const double gain=2*a*covariance-a*a*variance;
        return std::abs(gain-regularizer)<1e-9*std::max(1.,std::abs(gain)+regularizer);
    };
    for(int c=0;c<2;++c){
        if(border(s.cross[c]-s.sum*s.color[c]/count,variance,count*4))return true;
        if(tile==64&&border(s.detailCross[c],s.detailVariance,s.detailCount*.25))return true;
    }
    return false;
}
GpuColorStatistics orderedColorStatistics(const WaveletSourceColor& source,uint32_t tile,uint32_t cell){
    GpuColorStatistics s;
    const uint32_t cols=(source.width+tile-1)/tile,stride=(source.width+63)&~63u;
    const uint32_t x0=(cell%cols)*tile,y0=(cell/cols)*tile;
    const uint32_t right=std::min(source.width,x0+tile),bottom=std::min(source.height,y0+tile);
    for(uint32_t y=y0;y<bottom;++y)for(uint32_t x=x0;x<right;++x){
        const auto at=size_t(y)*stride+x;const double l=source.planes[0][at];
        s.sum+=l;s.square+=l*l;
        for(int c=0;c<2;++c){const double color=source.planes[c+1][at];s.color[c]+=color;s.cross[c]+=l*color;}++s.count;
    }
    if(tile==64)for(uint32_t y=y0;y<bottom;y+=2)for(uint32_t x=x0;x<right;x+=2){
        const size_t at[4]={size_t(y)*stride+x,size_t(y)*stride+std::min(source.width-1,x+1),
            size_t(std::min(source.height-1,y+1))*stride+x,size_t(std::min(source.height-1,y+1))*stride+std::min(source.width-1,x+1)};
        double mean[3]{};for(int c=0;c<3;++c)for(auto i:at)mean[c]+=source.planes[c][i]*.25;
        for(auto i:at){const double dy=source.planes[0][i]-mean[0];s.detailVariance+=dy*dy;
            for(int c=0;c<2;++c)s.detailCross[c]+=dy*(source.planes[c+1][i]-mean[c+1]);}s.detailCount+=4;
    }
    return s;
}
ColorModel fitCentered(const Frame& f, uint32_t tile, uint32_t threads, uint8_t colorSpace = 0, bool fast = false,
                       WaveletSourceColor* sourceColor = nullptr) {
    const uint32_t cols = (f.width + tile - 1) / tile, rows = (f.height + tile - 1) / tile;
    ColorModel model{Bytes(size_t(cols) * rows * 2), std::vector<float>(size_t(cols) * rows)};
    const uint32_t stride = (f.width + 63) & ~63u, paddedHeight = (f.height + 63) & ~63u;
    if (sourceColor) {
        sourceColor->fittedPatches.clear();
        require(colorSpace == 2 && (!fast || tile % 4 == 0), "Invalid shared color predictor profile.");
        sourceColor->width = f.width;
        sourceColor->height = f.height;
        sourceColor->fast = fast;
        for (auto& p : sourceColor->planes)
            p.resize(size_t(stride) * paddedHeight);
    }
    bool gpuConverted=false;
    if(computeBackend()==ComputeBackend::Gpu&&sourceColor&&fast&&colorSpace==2){
        std::array<GpuPlane,3> planes{{{sourceColor->planes[0],stride,paddedHeight},{sourceColor->planes[1],stride,paddedHeight},{sourceColor->planes[2],stride,paddedHeight}}};
        std::vector<GpuColorStatistics> fitted(size_t(cols)*rows);
        if(tile==64)sourceColor->fittedPatches.resize(size_t(cols+1)*(rows+1));
        if(gpuSourceColorModel(f,planes,tile,fitted,sourceColor->fittedPatches)){
            // Only a small cell summary is added to the existing color-plane
            // readback. Preserve CPU quantization and signed tie rounding.
            for(size_t cell=0;cell<fitted.size();++cell){
                if(colorFitBorder(fitted[cell],tile))fitted[cell]=orderedColorStatistics(*sourceColor,tile,uint32_t(cell));
                const auto& s=fitted[cell];const double count=s.count;
                const double variance=std::max(0.,s.square-s.sum*s.sum/count);
                model.means[cell]=float(s.sum/count);
                for(int c=0;c<2;++c){
                    const double cov=s.cross[c]-s.sum*s.color[c]/count;
                    const int q=std::clamp(int(std::lround(cov/(variance+count*4)*16)),-24,24);
                    const double a=q/16.;
                    int value=2*a*cov-a*a*variance>count*4?q:0;
                    if(tile==64){
                        const int dq=std::clamp(int(std::lround(s.detailCross[c]/(s.detailVariance+s.detailCount*.25)*16)),-24,24);
                        const double da=dq/16.;
                        const int detail=2*da*s.detailCross[c]-da*da*s.detailVariance>s.detailCount*.25?dq:0;
                        value=int(std::lround((value+detail)*.5));
                    }
                    model.map[cell*2+c]=uint8_t(int8_t(value));
                }
            }
            return model;
        }
        sourceColor->fittedPatches.clear();
        gpuConverted=gpuSourceColor(f,planes);
    }
    parallelFor(rows, threads, [&](uint32_t ty) {
        for (uint32_t tx = 0; tx < cols; ++tx) {
            double sy = 0, syy = 0, sc[2]{}, syc[2]{};
            uint32_t count = 0;
            auto accumulate = [&](double yy, double cb, double cr) {
                sy += yy;
                syy += yy * yy;
                sc[0] += cb;
                sc[1] += cr;
                syc[0] += yy * cb;
                syc[1] += yy * cr;
                ++count;
            };
            const uint32_t right = std::min(f.width, (tx + 1) * tile), bottom = std::min(f.height, (ty + 1) * tile);
            const auto mask = _mm_set1_epi32(255);
            if(gpuConverted){
                for(uint32_t y=ty*tile;y<bottom;++y)for(uint32_t x=tx*tile;x<right;++x){const auto at=size_t(y)*stride+x;
                    accumulate(sourceColor->planes[0][at],sourceColor->planes[1][at],sourceColor->planes[2][at]);}
            }else if (colorSpace >= 3) {
                const int axis = colorSpace - 3;
                for (uint32_t y = ty * tile; y < bottom; ++y)
                    for (uint32_t x = tx * tile; x < right; ++x) {
                        const auto* p = f.pixels.data() + (size_t(y) * f.width + x) * 4;
                        accumulate(float(p[axis]) - 128.f, .5f * (int(p[(axis + 1) % 3]) - int(p[axis])),
                                   .5f * (int(p[(axis + 2) % 3]) - int(p[axis])));
                    }
            } else if (colorSpace >= 2) {
                for (uint32_t y = ty * tile; y < bottom; ++y) {
                    uint32_t x = tx * tile;
                    if (fast)
                        for (; x + 4 <= right; x += 4) {
                            __m128 l, a, b;
                            perceptualColor::forward4(f.pixels.data() + (size_t(y) * f.width + x) * 4, l, a, b);
                            alignas(16) float ll[4], aa[4], bb[4];
                            _mm_store_ps(ll, l);
                            _mm_store_ps(aa, a);
                            _mm_store_ps(bb, b);
                            if (sourceColor) {
                                const size_t at = size_t(y) * stride + x;
                                _mm_storeu_ps(sourceColor->planes[0].data() + at, l);
                                _mm_storeu_ps(sourceColor->planes[1].data() + at, a);
                                _mm_storeu_ps(sourceColor->planes[2].data() + at, b);
                            }
                            for (int i = 0; i < 4; ++i)
                                accumulate(ll[i], aa[i], bb[i]);
                        }
                    for (; x < right; ++x) {
                        const auto lab = perceptualColor::forward(f.pixels.data() + (size_t(y) * f.width + x) * 4);
                        if (sourceColor)
                            for (int c = 0; c < 3; ++c)
                                sourceColor->planes[c][size_t(y) * stride + x] = lab[c];
                        accumulate(lab[0], lab[1], lab[2]);
                    }
                }
            } else if (colorSpace == 1) {
                for (uint32_t y = ty * tile; y < bottom; ++y)
                    for (uint32_t x = tx * tile; x < right; ++x) {
                        const auto* p = f.pixels.data() + (size_t(y) * f.width + x) * 4;
                        accumulate(.25f * p[0] + .5f * p[1] + .25f * p[2] - 128.f, .5f * (p[0] - p[2]),
                                   .5f * p[1] - .25f * (p[0] + p[2]));
                    }
            } else
                for (uint32_t y = ty * tile; y < bottom; ++y) {
                    uint32_t x = tx * tile;
                    for (; x + 4 <= right; x += 4) {
                        const auto rgba = _mm_loadu_si128(
                            reinterpret_cast<const __m128i*>(f.pixels.data() + (size_t(y) * f.width + x) * 4));
                        const auto r = _mm_cvtepi32_ps(_mm_and_si128(rgba, mask)),
                                   g = _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(rgba, 8), mask)),
                                   b = _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(rgba, 16), mask));
                        alignas(16) float yy[4], cb[4], cr[4];
                        _mm_store_ps(yy, _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(r, _mm_set1_ps(.299f)),
                                                                          _mm_mul_ps(g, _mm_set1_ps(.587f))),
                                                               _mm_mul_ps(b, _mm_set1_ps(.114f))),
                                                    _mm_set1_ps(128)));
                        _mm_store_ps(cb, _mm_add_ps(_mm_sub_ps(_mm_mul_ps(r, _mm_set1_ps(-.168736f)),
                                                               _mm_mul_ps(g, _mm_set1_ps(.331264f))),
                                                    _mm_mul_ps(b, _mm_set1_ps(.5f))));
                        _mm_store_ps(cr, _mm_sub_ps(_mm_sub_ps(_mm_mul_ps(r, _mm_set1_ps(.5f)),
                                                               _mm_mul_ps(g, _mm_set1_ps(.418688f))),
                                                    _mm_mul_ps(b, _mm_set1_ps(.081312f))));
                        for (int i = 0; i < 4; ++i)
                            accumulate(yy[i], cb[i], cr[i]);
                    }
                    for (; x < right; ++x) {
                        const auto* p = f.pixels.data() + (size_t(y) * f.width + x) * 4;
                        accumulate(.299f * p[0] + .587f * p[1] + .114f * p[2] - 128.f,
                                   -.168736f * p[0] - .331264f * p[1] + .5f * p[2],
                                   .5f * p[0] - .418688f * p[1] - .081312f * p[2]);
                    }
                }
            const double variance = std::max(0., syy - sy * sy / count);
            const size_t cell = size_t(ty) * cols + tx;
            model.means[cell] = float(sy / count);
            for (int c = 0; c < 2; ++c) {
                const double cov = syc[c] - sy * sc[c] / count;
                const int q = std::clamp(int(std::lround(cov / (variance + count * 4) * 16)), -24, 24);
                const double a = q / 16.;
                model.map[cell * 2 + c] = uint8_t(int8_t(2 * a * cov - a * a * variance > count * 4 ? q : 0));
            }
            if(sourceColor&&tile==64)refineDetailCell(f,tile,stride,uint32_t(cell),model,*sourceColor);
        }
    });
    if (sourceColor && !gpuConverted) {
        // Padding uses the precise scalar conversion, as the original wavelet
        // conversion did, even when valid pixels use the faster SIMD path.
        if (stride > f.width)
            parallelFor(f.height, threads, [&](uint32_t y) {
                const auto lab = perceptualColor::forward(f.pixels.data() + (size_t(y) * f.width + f.width - 1) * 4);
                for (int c = 0; c < 3; ++c)
                    std::fill(sourceColor->planes[c].begin() + size_t(y) * stride + f.width,
                              sourceColor->planes[c].begin() + size_t(y + 1) * stride, lab[c]);
            });
        parallelFor(paddedHeight - f.height, threads, [&](uint32_t row) {
            for (auto& p : sourceColor->planes)
                std::copy_n(p.data() + size_t(f.height - 1) * stride, stride,
                            p.data() + size_t(f.height + row) * stride);
        });
    }
    return model;
}

} // namespace
Packet encodeColorPrediction(const Frame& f, const EncodeOptions& options, bool opaque) {
    f.validate();
    auto o = options;
    o.automaticTarget = false;
    const uint32_t tile = options.colorModelTile;
    require(tile == 32 || tile == 64, "Invalid color-model tile.");
    // The current centered predictor is tuned for subsampled chroma. A user
    // request for full chroma goes directly through the existing 4:4:4 wavelet
    // path; it must never silently turn back into 4:2:0.
    if (o.chroma422 || !o.chroma420 || !opaque || (o.targetBytes && o.targetBytes < 4096) || f.width < 64 || f.height < 64)
        return encodeWavelet(f, o);
    TimingTimeline timing;
    thread_local WaveletSourceColor cached;
    WaveletSourceColor owned;
    auto& source = uint64_t((f.width + 63) & ~63u) * ((f.height + 63) & ~63u) <= 8 * 1024 * 1024 ? cached : owned;
    auto model = fitCentered(f, tile, o.threads, 2, true, &source);
    source.knownOpaque = true;
    for (auto& value : model.map)
        value = uint8_t(int8_t(std::lround(int8_t(value) * .75)));
    if(tile==64)refineInterpolatedChroma(source,tile,o.threads,model.map);
    timing.mark("encode.r6_model_fit");
    auto coded = entropyEncode(model.map, true);
    uint16_t mode = 1;
    const uint32_t cols = (f.width + tile - 1) / tile;
    Bytes deltas(model.map.size());
    for (size_t cell = 0; cell < model.map.size() / 2; ++cell)
        for (int c = 0; c < 2; ++c) {
            const size_t at = cell * 2 + c;
            const int left = cell % cols ? model.map[at - 2] : 0, top = cell >= cols ? model.map[at - cols * 2] : 0,
                      corner = cell >= cols && cell % cols ? model.map[at - cols * 2 - 2] : 0;
            deltas[at] = uint8_t(model.map[at] - medianPredict(left, top, corner));
        }
    auto predicted = entropyEncode(deltas, true);
    if (predicted.size() < coded.size()) {
        coded = std::move(predicted);
        mode |= 4;
    }
    timing.mark("encode.r6_model_coding");
    const uint32_t overhead = 7 + encodedVarSize(uint32_t(coded.size())) + uint32_t(coded.size());
    require(!o.targetBytes || o.targetBytes > overhead + 256, "Color model exceeds target.");
    if (o.targetBytes)
        o.targetBytes -= overhead;
    auto inner = encodeWavelet(f, o, true, model.map, tile, model.means, &source);
    ByteWriter out;
    out.data.reserve(overhead + inner.data.size());
    out.u32(frameMagic);
    out.u8(55);
    out.u8(uint8_t(tile));
    out.u8(uint8_t(mode));
    out.var(uint32_t(coded.size()));
    out.bytes(coded);
    out.bytes(inner.data);
    require(out.data.size() <= maxPacket && (!options.targetBytes || out.data.size() <= options.targetBytes),
            "Color prediction exceeds target.");
    return {std::move(out.data), f.pts, f.duration};
}
Frame decodeColorPrediction(PacketView packet, uint32_t width, uint32_t height, uint32_t threads,
                            std::span<uint8_t> destination, uint32_t deblocking, uint32_t sharpening) {
    pixelBytes(width, height);
    require(packet.data.size() <= maxPacket, "Color packet too large.");
    ByteReader in{packet.data};
    require(in.u32() == frameMagic && in.u8() == 55, "Unsupported color prediction format.");
    const uint32_t tile = in.u8(), mode = in.u8();
    require((tile == 32 || tile == 64) && (mode == 1 || mode == 5), "Invalid color predictor.");
    auto coded = in.bytes(in.var()), child = in.bytes(in.data.size() - in.pos);
    in.end();
    const size_t cells = size_t((width + tile - 1) / tile) * ((height + tile - 1) / tile) * 2;
    auto map = entropyDecode(coded, cells);
    require(map.size() == cells, "Invalid color model size.");
    if (mode & 4) {
        const uint32_t cols = (width + tile - 1) / tile;
        for (size_t cell = 0; cell < cells / 2; ++cell)
            for (int c = 0; c < 2; ++c) {
                const auto at = cell * 2 + c;
                const int left = cell % cols ? map[at - 2] : 0, top = cell >= cols ? map[at - cols * 2] : 0,
                          corner = cell >= cols && cell % cols ? map[at - cols * 2 - 2] : 0;
                map[at] = uint8_t(map[at] + medianPredict(left, top, corner));
            }
    }
    for (auto v : map)
        require(int8_t(v) >= -32 && int8_t(v) <= 32, "Invalid color slope.");
    require(child.size() >= 32 && child[4] == 114, "Unsupported color residual.");
    // Centered CfL retains its reviewed interpolation (smoothing=0), while the
    // caller still controls saved native cleanup and optional contrast recovery.
    return decodeWavelet({child, packet.pts, packet.duration}, width, height, threads, destination, 0, sharpening, {}, nullptr,
                         false, map, tile, true, 0, deblocking);
}
} // namespace fajk
