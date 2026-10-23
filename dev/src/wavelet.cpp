#include "wavelet_debug.hpp"
#include "adaptive_allocation.hpp"
#include "wavelet.hpp"
#include "gpu_compute.hpp"
#include "wavelet_adjoint.hpp"
#include "contrast_loss.hpp"
#include "perceptual_color.hpp"
#include "color_prediction_rows.hpp"
#include "chroma_precomp.hpp"
#include "spatial.hpp"
#include "native_restoration.hpp"
#include "wavelet_profile.hpp"
#include "rgb_restoration.hpp"
#include "flat_regions.hpp"
#include "codec_internal.hpp"
#include "fajk/hardware.hpp"
#include "timing.hpp"
#include "source_priority.hpp"
#include "guided_chroma.hpp"
#include "chroma_cleanup.hpp"
#include "adaptive_chroma.hpp"
#include "approx_pilot.hpp"
#include <cstring>
#include <bit>
#include <memory>
#include <cstdlib>

namespace fajk {
bool hasAvx2();
bool waveletOpaqueAvx2(const uint8_t*, size_t);
void waveletHistogramAvx2(const float*, int, float, uint32_t, uint32_t*);
bool waveletQuantize16Avx2(const float*, float, int*, float = .35f);
uint32_t waveletProxyColorRowAvx2(const float*, const float*, const float*, int, int, uint32_t, uint32_t, bool,
                                  uint32_t, uint8_t*, const float*, const float*, uint8_t);
uint32_t waveletColorRowAvx2(const float*, const float*, const float*, int, int, uint32_t, uint32_t, bool, uint32_t,
                             uint8_t*, const float* = nullptr, const float* = nullptr, float = 0,
                             const uint8_t* = nullptr, float = 0, const uint8_t* = nullptr, const uint32_t* = nullptr,
                             uint32_t = 16, const float* = nullptr, const float* = nullptr);
void waveletRowAvx2(float*, float*, int, bool);
void waveletColumnRangeAvx2(float*, float*, int, int, int, int, int, bool);
namespace {
constexpr float alpha = -1.586134342f, beta = -.05298011854f, gamma = .8829110762f, delta = .4435068522f,
                normal = 1.149604398f;
constexpr int levels = 6;
constexpr size_t cachedSamples = 8 * 1024 * 1024;

// Independent stripes keep the largest detail bands from serializing the
// entropy decoder. Boundaries follow 16-row contexts and do not depend on
// worker count, so the packet is deterministic.
int spatialRows(int width, int height, int parts = 4) {
    return parts > 1 && uint64_t(width) * height >= 65536 ? ((height + parts - 1) / parts + 15) & ~15 : height;
}

void liftRow(float* dst, const float* neighbor, int count, float factor, bool next) {
    const auto f = _mm_set1_ps(factor);
    int i = 0;
    if (!next) {
        dst[0] += factor * (neighbor[0] + neighbor[0]);
        i = 1;
    }
    for (; i + 4 < (next ? count : count + 1); i += 4) {
        auto a = _mm_loadu_ps(neighbor + i), b = _mm_loadu_ps(neighbor + i + (next ? 1 : -1));
        _mm_storeu_ps(dst + i, _mm_add_ps(_mm_loadu_ps(dst + i), _mm_mul_ps(f, _mm_add_ps(a, b))));
    }
    for (; i < count; ++i)
        dst[i] += factor * (neighbor[i] + neighbor[std::clamp(i + (next ? 1 : -1), 0, count - 1)]);
}
void row(float* samples, int count, bool inverse, bool allowAvx2) {
    thread_local std::vector<float> scratch;
    scratch.resize(count);
    if (allowAvx2 && count >= 32) {
        waveletRowAvx2(samples, scratch.data(), count, inverse);
        return;
    }
    const int half = count / 2;
    auto* lo = scratch.data();
    auto* hi = lo + half;
    if (inverse) {
        std::copy_n(samples, count, lo);
        int i = 0;
        for (; i + 4 <= half; i += 4) {
            _mm_storeu_ps(lo + i, _mm_mul_ps(_mm_loadu_ps(lo + i), _mm_set1_ps(1 / normal)));
            _mm_storeu_ps(hi + i, _mm_mul_ps(_mm_loadu_ps(hi + i), _mm_set1_ps(normal)));
        }
        for (; i < half; ++i) {
            lo[i] /= normal;
            hi[i] *= normal;
        }
        liftRow(lo, hi, half, -delta, false);
        liftRow(hi, lo, half, -gamma, true);
        liftRow(lo, hi, half, -beta, false);
        liftRow(hi, lo, half, -alpha, true);
        i = 0;
        for (; i + 4 <= half; i += 4) {
            const auto a = _mm_loadu_ps(lo + i), b = _mm_loadu_ps(hi + i);
            _mm_storeu_ps(samples + i * 2, _mm_unpacklo_ps(a, b));
            _mm_storeu_ps(samples + i * 2 + 4, _mm_unpackhi_ps(a, b));
        }
        for (; i < half; ++i) {
            samples[i * 2] = lo[i];
            samples[i * 2 + 1] = hi[i];
        }
    } else {
        int i = 0;
        for (; i + 4 <= half; i += 4) {
            const auto a = _mm_loadu_ps(samples + i * 2), b = _mm_loadu_ps(samples + i * 2 + 4);
            _mm_storeu_ps(lo + i, _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)));
            _mm_storeu_ps(hi + i, _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1)));
        }
        for (; i < half; ++i) {
            lo[i] = samples[i * 2];
            hi[i] = samples[i * 2 + 1];
        }
        liftRow(hi, lo, half, alpha, true);
        liftRow(lo, hi, half, beta, false);
        liftRow(hi, lo, half, gamma, true);
        liftRow(lo, hi, half, delta, false);
        i = 0;
        for (; i + 4 <= half; i += 4) {
            _mm_storeu_ps(samples + i, _mm_mul_ps(_mm_loadu_ps(lo + i), _mm_set1_ps(normal)));
            _mm_storeu_ps(samples + half + i, _mm_mul_ps(_mm_loadu_ps(hi + i), _mm_set1_ps(1 / normal)));
        }
        for (; i < half; ++i) {
            samples[i] = lo[i] * normal;
            samples[half + i] = hi[i] / normal;
        }
    }
}

// Vertical lifting processes adjacent columns together; no strided pixel gathers.
void columnStage(std::span<float> image, std::vector<float>& scratch, int stride, int w, int h, uint32_t threads,
                 bool inverse, bool allowAvx2) {
    const int half = h / 2;
    if (allowAvx2 && hasAvx2() && w >= 8) {
        constexpr int strip = 64;
        parallelFor(uint32_t((w + strip - 1) / strip), threads, [&](uint32_t job) {
            const int begin = int(job) * strip;
            waveletColumnRangeAvx2(image.data(), scratch.data(), stride, w, h, begin, std::min(w, begin + strip),
                                   inverse);
        });
        return;
    }
    parallelFor(uint32_t((w + 63) / 64), threads, [&](uint32_t job) {
        const int begin = int(job) * 64, end = std::min(w, begin + 64);
        for (int y = 0; y < h; ++y) {
            const int source = inverse ? y : (y < half ? y * 2 : (y - half) * 2 + 1);
            std::copy(image.data() + size_t(source) * stride + begin, image.data() + size_t(source) * stride + end,
                      scratch.data() + size_t(y) * w + begin);
        }
        auto lift = [&](bool low, float factor) {
            const int target = low ? 0 : half, other = low ? half : 0;
            for (int y = 0; y < half; ++y) {
                auto* dst = scratch.data() + size_t(target + y) * w;
                const auto* a = scratch.data() + size_t(other + y) * w;
                const auto* b = scratch.data() + size_t(other + std::clamp(y + (low ? -1 : 1), 0, half - 1)) * w;
                const auto f = _mm_set1_ps(factor);
                int x = begin;
                for (; x + 4 <= end; x += 4)
                    _mm_storeu_ps(dst + x,
                                  _mm_add_ps(_mm_loadu_ps(dst + x),
                                             _mm_mul_ps(f, _mm_add_ps(_mm_loadu_ps(a + x), _mm_loadu_ps(b + x)))));
                for (; x < end; ++x)
                    dst[x] += factor * (a[x] + b[x]);
            }
        };
        auto scale = [&](float low, float high) {
            for (int y = 0; y < h; ++y) {
                auto* dst = scratch.data() + size_t(y) * w;
                const auto factor = _mm_set1_ps(y < half ? low : high);
                int x = begin;
                for (; x + 4 <= end; x += 4)
                    _mm_storeu_ps(dst + x, _mm_mul_ps(_mm_loadu_ps(dst + x), factor));
                for (; x < end; ++x)
                    dst[x] *= y < half ? low : high;
            }
        };
        if (inverse) {
            scale(1 / normal, normal);
            lift(true, -delta);
            lift(false, -gamma);
            lift(true, -beta);
            lift(false, -alpha);
        } else {
            lift(false, alpha);
            lift(true, beta);
            lift(false, gamma);
            lift(true, delta);
            scale(normal, 1 / normal);
        }
        for (int y = 0; y < h; ++y) {
            const int source = inverse ? (y % 2 ? half + y / 2 : y / 2) : y;
            std::copy(scratch.data() + size_t(source) * w + begin, scratch.data() + size_t(source) * w + end,
                      image.data() + size_t(y) * stride + begin);
        }
    });
}

// Schedule each inverse stage across all independent CDF 9/7 planes in one
// batch. No nested plane/row pools or changes to floating-point evaluation.
void inverseCdfPlanes(std::array<std::vector<float>, 3>& planes, int w, int h, int cw, int ch, uint32_t threads,
                      bool modernRows) {
    thread_local std::array<std::vector<float>, 3> cached;
    std::array<std::vector<float>, 3> large;
    std::array<std::vector<float>*, 3> scratch{};
    for (unsigned c = 0; c < 3; ++c) {
        scratch[c] = planes[c].size() <= cachedSamples ? &cached[c] : &large[c];
        if (scratch[c]->size() < planes[c].size())
            scratch[c]->resize(planes[c].size());
    }
    for (int level = levels - 1; level >= 0; --level) {
        const std::array<int, 3> stride{w, cw, cw}, rw{w >> level, cw >> level, cw >> level},
            rh{h >> level, ch >> level, ch >> level};
        const uint32_t workers = uint64_t(rw[0]) * rh[0] <= 65536 ? 1 : threads;
        std::array<uint32_t, 4> columns{}, rows{};
        for (unsigned c = 0; c < 3; ++c) {
            columns[c + 1] = columns[c] + uint32_t(rw[c] >= 8 ? (rw[c] + 63) / 64 : 1);
            rows[c + 1] = rows[c] + uint32_t((rh[c] + 15) / 16);
        }
        parallelFor(columns[3], workers, [&](uint32_t job) {
            const unsigned c = job < columns[1] ? 0 : job < columns[2] ? 1 : 2;
            const int begin = int(job - columns[c]) * 64;
            if (rw[c] >= 8)
                waveletColumnRangeAvx2(planes[c].data(), scratch[c]->data(), stride[c], rw[c], rh[c], begin,
                                       std::min(rw[c], begin + 64), true);
            else
                columnStage(planes[c], *scratch[c], stride[c], rw[c], rh[c], 1, true, false);
        });
        parallelFor(rows[3], workers, [&](uint32_t job) {
            const unsigned c = job < rows[1] ? 0 : job < rows[2] ? 1 : 2;
            const int begin = int(job - rows[c]) * 16;
            for (int y = begin; y < std::min(rh[c], begin + 16); ++y)
                row(planes[c].data() + size_t(y) * stride[c], rw[c], true, modernRows);
        });
    }
}

float stepFor(float base, int level, int band, bool detail = false) {
    if (band < 0)
        return std::max(.25f, base * .10f);
    return std::max(.35f, base * (band == 2 ? 1.35f : 1.f) * std::pow(detail ? .65f : .83f, float(level)));
}

template <class F> void bands(int w, int h, F&& f) {
    for (int level = 0; level < levels; ++level) {
        int rw = w >> level, rh = h >> level, hw = rw / 2, hh = rh / 2;
        f(level, 0, hw, 0, hw, hh);
        f(level, 1, 0, hh, hw, hh);
        f(level, 2, hw, hh, hw, hh);
    }
}
// Fine-band correction is independent across bands and rows. Dispatch one
// batch instead of twelve thread-pool barriers, including tiny coarse bands.
template <class F> void parallelFineBands(int w, int h, uint32_t threads, F&& work) {
    struct Stripe {
        int level, band, bx, by, bw, row, count;
    };
    std::vector<Stripe> stripes;
    bands(w, h, [&](int level, int band, int bx, int by, int bw, int bh) {
        if (level >= 4)
            return;
        for (int row = 0; row < bh; row += 16)
            stripes.push_back({level, band, bx, by, bw, row, std::min(16, bh - row)});
    });
    parallelFor(uint32_t(stripes.size()), threads, [&](uint32_t job) {
        const auto& s = stripes[job];
        for (int row = s.row; row < s.row + s.count; ++row)
            work(s.level, s.band, s.bx, s.by, s.bw, row);
    });
}
} // namespace

void waveletTransform(std::span<float> samples, int stride, int height, uint32_t threads, bool inverse,
                      bool allowAvx2, bool allowRowAvx2, int firstLevel, int levelCount) {
    require(firstLevel >= 0 && levelCount > 0 && firstLevel + levelCount <= 6, "Invalid wavelet transform levels.");
    require(stride >= 64 && height >= 64 && stride % 64 == 0 && height % 64 == 0 &&
                samples.size() == size_t(stride) * height,
            "Invalid wavelet dimensions.");
    thread_local std::vector<float> cachedScratch;
    std::vector<float> largeScratch;
    auto& scratch = samples.size() <= cachedSamples ? cachedScratch : largeScratch;
    if (scratch.size() < samples.size())
        scratch.resize(samples.size());
    allowAvx2 = allowAvx2 && hasAvx2();
    for (int i = 0; i < levelCount; ++i) {
        const int level = inverse ? firstLevel + levelCount - 1 - i : firstLevel + i, w = stride >> level,
                  h = height >> level;
        const uint32_t workers = uint64_t(w) * h <= 65536 ? 1 : threads;
        if (inverse)
            columnStage(samples, scratch, stride, w, h, workers, true, allowAvx2);
        parallelFor(uint32_t((h + 15) / 16), workers, [&](uint32_t job) {
            for (int y = int(job) * 16; y < std::min(h, int(job) * 16 + 16); ++y)
                row(samples.data() + size_t(y) * stride, w, inverse, allowAvx2 && allowRowAvx2);
        });
        if (!inverse)
            columnStage(samples, scratch, stride, w, h, workers, false, allowAvx2);
    }
}

namespace {
int nativeKernelForChannel(int code, int) {
    require(code == 10, "Only current CDF 9/7 is supported.");
    return code;
}
void transformNativePlane(std::vector<float>& plane, int stride, int height, uint32_t threads, bool inverse, int code,
                          bool modernRows = true) {
    require(code == 10, "Only current CDF 9/7 is supported.");
    waveletTransform(plane, stride, height, threads, inverse, true, modernRows);
}
} // namespace

namespace {
struct SynthesisSourceCache {
    std::array<std::vector<float>, 3> transformed;
    std::vector<float> spatialLuma;
    std::unique_ptr<SourcePriority> importance;
    uint8_t importanceMode = 0;
    Bytes alpha;
    bool opaque = true, ready = false;
};
} // namespace

static Packet encodeWaveletImpl(const Frame& f, const EncodeOptions& o, bool allowFlatRegions,
                                std::span<const uint8_t> cflMap, uint32_t cflTile, std::span<const float> cflMeans,
                                SynthesisSourceCache* sourceOutput = nullptr, int ratePassOverride = -2,
                                WaveletSourceColor* sourceColor = nullptr) {
    require(!(o.chroma420 && o.chroma422), "Conflicting chroma sampling modes.");
    TimingTimeline timing;
    const bool spatialCoding = true, predictionExperiment = false, strengthSweep = false, allocationSweep = false;
    const int defaultPredictionLevel = 4;
    const bool coarseAllocation = false, filterPlaneSweep = false, adaptiveHybrid = false, hybridReconstruction = false;
    const uint8_t nativeFilterPlanes = 3;
    const bool lowRate = o.targetBytes && uint64_t(o.targetBytes) * 1000 < uint64_t(f.width) * f.height * 35;
    const bool r12 = true;
    auto r12Profile = resolveWaveletProfile(o.waveletStage, lowRate);
    if (lowRate) {
        r12Profile.chroma = uint8_t(std::max(32, int(r12Profile.chroma) * 3 / 4));
        r12Profile.adaptiveRate *= .5f;
        r12Profile.sourcePriority = 31;
        r12Profile.quantRdo = .01f;
        r12Profile.quantGradient = .125f;
        r12Profile.adaptive = 0;
        r12Profile.rateQuantizer = false;
    }
    if (lowRate && o.adaptiveQuantizationEnabled) {
        r12Profile.entropyMode = 32;
        r12Profile.adaptive = 3;
        r12Profile.coupledQuantizer = 0;
        r12Profile.rateQuantizer = true;
    }
    if (!o.adaptiveQuantizationEnabled) {
        r12Profile.sourcePriority = 0;
        r12Profile.adaptive = 0;
        r12Profile.coupledQuantizer = 0;
        r12Profile.rateQuantizer = false;
    }
    // The pilot pays for itself at very low density. As coefficients become
    // more accurate, taper this heuristic pre-bias instead of paying for a
    // quantize/inverse/adjoint pilot whose remaining benefit is negligible.
    if (o.targetBytes && r12Profile.synthesisGain) {
        const double density = double(o.targetBytes) / (double(f.width) * f.height);
        const float t = std::clamp(float((density - .015) / .010), 0.f, 1.f);
        r12Profile.synthesisGain *= 1 - t * t * (3 - 2 * t);
    }
    // Allocation changes source RDO prices, not the bitstream or candidate
    // lattice. Switching to mode 24 away from 50 added precision signalling
    // and disabled the low-rate synthesis pilot, undoing the intended savings.
    if (!lowRate && o.waveletStage == WaveletStage::Final)
        r12Profile.reuseSynthesisSource = true;
    if (r12Profile.synthesisGain) {
        r12Profile.approximatePilot = 1;
        r12Profile.reuseSynthesisSource = false;
        r12Profile.pilotStepScale = lowRate ? 1.25f : 1.f;
    }
    if (ratePassOverride >= 0)
        r12Profile.ratePasses = ratePassOverride;
    if(o.speed==EncodeSpeed::Fast){
        // Keep source priorities, AQ, reconstruction and the packet syntax.
        // Replace context-priced local precision/RDO with the existing cheap
        // precision estimator and skip optional synthesis pilot corrections.
        r12Profile.rateQuantizer=false;r12Profile.coupledQuantizer=0;
        r12Profile.quantRdo=0;r12Profile.quantGradient=0;r12Profile.quantCoupling=0;
        r12Profile.synthesisGain=0;r12Profile.approximatePilot=0;r12Profile.reuseSynthesisSource=false;
        r12Profile.ratePasses=1;r12Profile.fitRateCap=true;
    }
    const bool decodedPilot = lowRate && r12Profile.synthesisGain && (r12Profile.adaptive == 0 || r12Profile.adaptive == 3) &&
                              o.waveletStage == WaveletStage::Final;
    SynthesisSourceCache synthesisSource;
    std::vector<float> synthesisDecoded, synthesisGradient;
    ContrastLoss contrast;
    Frame colorSeed;
    if ((r12Profile.synthesisGain || r12Profile.colorFeedback) && !r12Profile.approximatePilot) {
        auto seed = o;
        seed.budgetHeatmap.reset();
        seed.waveletStage = WaveletStage::Pilot;
        if (r12Profile.reuseSynthesisSource) {
            const auto seedProfile = resolveWaveletProfile(seed.waveletStage, lowRate);
            require(!seedProfile.synthesisGain && !seedProfile.colorFeedback &&
                        seedProfile.colorSpace == r12Profile.colorSpace &&
                        seedProfile.fastColor == r12Profile.fastColor && seedProfile.kernel == r12Profile.kernel,
                    "Invalid synthesis source reuse profile.");
        }
        const auto packet = encodeWaveletImpl(f, seed, allowFlatRegions, cflMap, cflTile, cflMeans,
                                              r12Profile.reuseSynthesisSource ? &synthesisSource : nullptr,
                                              r12Profile.synthesisRatePasses, sourceColor);
        colorSeed = decodeWavelet(packet, f.width, f.height, o.threads, {}, 100, 0, {}, &synthesisDecoded,
                                  !r12Profile.colorFeedback, cflMap, cflTile, !cflMeans.empty(), 0);
        timing.mark("encode.synthesis_seed_decode");
    }
    const int predictionLevel = r12Profile.prediction, alternativeKernel = r12Profile.kernel;
    const bool guidedExperiment = false, restorationExperiment = false;
    const uint8_t nativeGuide = r12Profile.guide, nativeStrength = r12Profile.strength;
    const bool splitLumaHeader = r12Profile.lumaStrength || (r12Profile.colorSpace && r12Profile.entropyMode);
    const bool compactNative = true, spatialPrediction = true;
    f.validate();
    bool detail = o.targetBytes && uint64_t(o.targetBytes) * 1000 <= uint64_t(f.width) * f.height * 35;
    if (o.targetBytes && !detail) {
        // Keep the screen-oriented high-rate allocation on images dominated
        // by flat UI backgrounds; photographs benefit from the detail profile.
        uint32_t flat = 0, total = 0;
        for (uint32_t y = 0; y < f.height; y += 16)
            for (uint32_t x = 1 + (y / 16) % 16; x < f.width; x += 16) {
                const auto* p = f.pixels.data() + (size_t(y) * f.width + x) * 4;
                uint32_t a, b;
                std::memcpy(&a, p, 4);
                std::memcpy(&b, p - 4, 4);
                flat += a == b;
                ++total;
            }
        detail = uint64_t(flat) * 100 < uint64_t(total) * 70;
    }
    const bool legacyRate = o.targetBytes && uint64_t(o.targetBytes) * 1000 < uint64_t(f.width) * f.height * 8;
    const bool balancedChroma = o.chroma420 && o.targetBytes;

    const bool r5 = true, partitionedSpatial = false, nativeR6 = true, nearest = true, r4 = false, tuned = true,
               guidedProfile = false, fastPhoto = true;
    const uint32_t nativeVariant = 4;
    const uint8_t slopeCode = r12Profile.slope, diagonalCode = r12Profile.diagonal, chromaCode = r12Profile.chroma,
                  restoration = 0;
    const float slope = slopeCode / 256.f, diagonal = diagonalCode / 128.f, chromaScale = chromaCode / 128.f;
    auto quantStep = [&](float base, int level, int band, bool) {
        return band < 0 ? std::max(.25f, base * .10f)
                        : std::max(.35f, base * (band == 2 ? diagonal : 1.f) * std::pow(slope, float(level)));
    };
    const int w = (int(f.width) + 63) & ~63, h = (int(f.height) + 63) & ~63, q = int(o.quality);
    thread_local std::array<std::vector<float>, 3> cachedPlanes;
    std::array<std::vector<float>, 3> largePlanes;
    auto& planes = size_t(w) * h <= cachedSamples ? cachedPlanes : largePlanes;
    const bool knownOpaqueSource =
        sourceColor && sourceColor->knownOpaque && sourceColor->width == f.width && sourceColor->height == f.height;
    const bool reuseSource = synthesisSource.ready;
    if (reuseSource)
        planes = std::move(synthesisSource.transformed);
    // Chroma must also support six levels, so it has its own padding.
    const int cw = (o.chroma420 || o.chroma422) ? ((int(f.width) + 1) / 2 + 63) & ~63 : w,
              ch = o.chroma420 ? ((int(f.height) + 1) / 2 + 63) & ~63 : h;
    for (int c = 0; c < 3; ++c)
        planes[c].resize(size_t(c ? cw : w) * (c ? ch : h));
    timing.mark("encode.prepare");
    const auto byteMask = _mm_set1_epi32(255);
    auto channel = [&](const uint8_t* data, int shift) {
        auto pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
        if (shift == 8)
            pixels = _mm_srli_epi32(pixels, 8);
        else if (shift == 16)
            pixels = _mm_srli_epi32(pixels, 16);
        return _mm_cvtepi32_ps(_mm_and_si128(pixels, byteMask));
    };
    bool gpuPrepared=false;
    if(!reuseSource&&knownOpaqueSource&&o.chroma420&&!o.chroma422&&cflTile==64&&!cflMap.empty()&&
        !r12Profile.synthesisGain&&!r12Profile.colorFeedback&&!r12Profile.cflPilotGain&&!sourceOutput&&
        computeBackend()==ComputeBackend::Gpu){
        std::array<GpuPlane,3> gpuPlanes{{{planes[0],uint32_t(w),uint32_t(h)},
            {planes[1],uint32_t(cw),uint32_t(ch)},{planes[2],uint32_t(cw),uint32_t(ch)}}};
        gpuPrepared=gpuSourceWavelet(f,gpuPlanes,cflTile,cflMap,cflMeans);
        if(gpuPrepared)sourceColor->width=0;
    }
    if (!reuseSource&&!gpuPrepared) {
        if (o.chroma422) {
            require(r12Profile.colorSpace == 2 && cflMap.empty(), "Unsupported 4:2:2 predictor.");
            std::array<std::vector<float>, 3> full;
            for (auto& v : full)
                v.resize(size_t(w) * h);
            perceptualColor::planes(f, full, w, h, w, h, false, o.threads, r12Profile.fastColor);
            planes[0].swap(full[0]);
            parallelFor(uint32_t(h), o.threads, [&](uint32_t y) {
                for (int x = 0; x < cw; ++x) {
                    const auto a = std::min(int(f.width) - 1, x * 2), b = std::min(int(f.width) - 1, a + 1);
                    for (int c = 1; c < 3; ++c)
                        planes[c][size_t(y) * cw + x] = (full[c][size_t(y) * w + a] + full[c][size_t(y) * w + b]) * .5f;
                }
            });
        } else if (r12Profile.colorSpace == 2) {
            if (sourceColor && sourceColor->width == f.width && sourceColor->height == f.height &&
                sourceColor->fast == r12Profile.fastColor && !sourceColor->planes[0].empty()) {
                for (const auto& p : sourceColor->planes)
                    require(p.size() == size_t(w) * h, "Invalid shared source color plane.");
                planes[0].swap(sourceColor->planes[0]);
                if (!o.chroma420) {
                    planes[1].swap(sourceColor->planes[1]);
                    planes[2].swap(sourceColor->planes[2]);
                } else
                    parallelFor(uint32_t(ch), o.threads, [&](uint32_t y) {
                        for (int x = 0; x < cw; ++x) {
                            const int ax = std::min(int(f.width) - 1, x * 2), bx = std::min(int(f.width) - 1, ax + 1),
                                      ay = std::min(int(f.height) - 1, int(y) * 2),
                                      by = std::min(int(f.height) - 1, ay + 1);
                            for (int c = 1; c < 3; ++c) {
                                const auto& p = sourceColor->planes[c];
                                planes[c][size_t(y) * cw + x] = ((p[size_t(ay) * w + ax] + p[size_t(ay) * w + bx]) +
                                                                 (p[size_t(by) * w + ax] + p[size_t(by) * w + bx])) *
                                                                .25f;
                            }
                        }
                    });
                sourceColor->width = 0; // The swapped storage now contains scratch.
            } else
                perceptualColor::planes(f, planes, w, h, cw, ch, o.chroma420, o.threads, r12Profile.fastColor);
        } else {
            parallelFor(uint32_t(h), o.threads, [&](uint32_t y) {
                const auto* source = f.pixels.data() + size_t(std::min(y, f.height - 1)) * f.width * 4;
                float* target = planes[0].data() + size_t(y) * w;
                uint32_t x = 0;
                for (; x + 4 <= f.width; x += 4) {
                    const auto r = channel(source + x * 4, 0), g = channel(source + x * 4, 8),
                               b = channel(source + x * 4, 16);
                    if (r12Profile.colorSpace >= 3) {
                        const __m128 v[3] = {r, g, b};
                        _mm_storeu_ps(target + x, _mm_sub_ps(v[r12Profile.colorSpace - 3], _mm_set1_ps(128)));
                    } else if (r12Profile.colorSpace)
                        _mm_storeu_ps(target + x, _mm_sub_ps(_mm_add_ps(_mm_mul_ps(_mm_add_ps(r, b), _mm_set1_ps(.25f)),
                                                                        _mm_mul_ps(g, _mm_set1_ps(.5f))),
                                                             _mm_set1_ps(128)));
                    else
                        _mm_storeu_ps(target + x, _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(r, _mm_set1_ps(.299f)),
                                                                                   _mm_mul_ps(g, _mm_set1_ps(.587f))),
                                                                        _mm_mul_ps(b, _mm_set1_ps(.114f))),
                                                             _mm_set1_ps(128)));
                }
                for (; x < uint32_t(w); ++x) {
                    const auto* p = source + std::min(x, f.width - 1) * 4;
                    target[x] = r12Profile.colorSpace >= 3 ? float(p[r12Profile.colorSpace - 3]) - 128.f
                                : r12Profile.colorSpace    ? .25f * p[0] + .5f * p[1] + .25f * p[2] - 128.f
                                                           : .299f * p[0] + .587f * p[1] + .114f * p[2] - 128.f;
                }
            });
            parallelFor(uint32_t(ch), o.threads, [&](uint32_t y) {
                int x = 0;
                float* cb = planes[1].data() + size_t(y) * cw;
                float* cr = planes[2].data() + size_t(y) * cw;
                const int scale = o.chroma420 ? 2 : 1;
                const auto* top = f.pixels.data() + size_t(std::min(uint32_t(y * scale), f.height - 1)) * f.width * 4;
                const auto* bottom =
                    f.pixels.data() + size_t(std::min(uint32_t(y * scale + scale - 1), f.height - 1)) * f.width * 4;
                for (; uint32_t((x + 4) * scale) <= f.width; x += 4) {
                    __m128 r, g, b;
                    if (scale == 1) {
                        r = channel(top + x * 4, 0);
                        g = channel(top + x * 4, 8);
                        b = channel(top + x * 4, 16);
                    } else {
                        auto sum = [&](int shift) {
                            const auto a = _mm_add_ps(channel(top + x * 8, shift), channel(bottom + x * 8, shift)),
                                       b = _mm_add_ps(channel(top + x * 8 + 16, shift),
                                                      channel(bottom + x * 8 + 16, shift));
                            return _mm_add_ps(_mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)),
                                              _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1)));
                        };
                        r = sum(0);
                        g = sum(8);
                        b = sum(16);
                    }
                    const auto norm = _mm_set1_ps(1.f / (scale * scale));
                    if (r12Profile.colorSpace >= 3) {
                        const __m128 v[3] = {r, g, b};
                        const int axis = r12Profile.colorSpace - 3;
                        _mm_storeu_ps(cb + x, _mm_mul_ps(_mm_sub_ps(v[(axis + 1) % 3], v[axis]),
                                                         _mm_mul_ps(norm, _mm_set1_ps(.5f))));
                        _mm_storeu_ps(cr + x, _mm_mul_ps(_mm_sub_ps(v[(axis + 2) % 3], v[axis]),
                                                         _mm_mul_ps(norm, _mm_set1_ps(.5f))));
                    } else if (r12Profile.colorSpace) {
                        _mm_storeu_ps(cb + x, _mm_mul_ps(_mm_sub_ps(r, b), _mm_mul_ps(norm, _mm_set1_ps(.5f))));
                        _mm_storeu_ps(cr + x, _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(g, _mm_set1_ps(.5f)),
                                                                    _mm_mul_ps(_mm_add_ps(r, b), _mm_set1_ps(.25f))),
                                                         norm));
                    } else {
                        _mm_storeu_ps(cb + x, _mm_mul_ps(_mm_add_ps(_mm_sub_ps(_mm_mul_ps(r, _mm_set1_ps(-.168736f)),
                                                                               _mm_mul_ps(g, _mm_set1_ps(.331264f))),
                                                                    _mm_mul_ps(b, _mm_set1_ps(.5f))),
                                                         norm));
                        _mm_storeu_ps(cr + x, _mm_mul_ps(_mm_sub_ps(_mm_sub_ps(_mm_mul_ps(r, _mm_set1_ps(.5f)),
                                                                               _mm_mul_ps(g, _mm_set1_ps(.418688f))),
                                                                    _mm_mul_ps(b, _mm_set1_ps(.081312f))),
                                                         norm));
                    }
                }
                for (; x < cw; ++x) {
                    float r = 0, g = 0, b = 0;
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx) {
                            const auto* p =
                                f.pixels.data() + (size_t(std::min(uint32_t(y * scale + dy), f.height - 1)) * f.width +
                                                   std::min(uint32_t(x * scale + dx), f.width - 1)) *
                                                      4;
                            r += p[0];
                            g += p[1];
                            b += p[2];
                        }
                    const float norm = 1.f / (scale * scale);
                    if (r12Profile.colorSpace >= 3) {
                        const float v[3] = {r, g, b};
                        const int axis = r12Profile.colorSpace - 3;
                        cb[x] = .5f * (v[(axis + 1) % 3] - v[axis]) * norm;
                        cr[x] = .5f * (v[(axis + 2) % 3] - v[axis]) * norm;
                        continue;
                    }
                    cb[x] = (r12Profile.colorSpace ? .5f * (r - b) : -.168736f * r - .331264f * g + .5f * b) * norm;
                    cr[x] = (r12Profile.colorSpace ? .5f * g - .25f * (r + b) : .5f * r - .418688f * g - .081312f * b) *
                            norm;
                }
            });
        }
        if (r12Profile.colorFeedback) {
            std::array<std::vector<float>, 3> decoded;
            for (int c = 0; c < 3; ++c)
                decoded[c].resize(planes[c].size());
            perceptualColor::planes(colorSeed, decoded, w, h, cw, ch, o.chroma420, o.threads);
            for (int c = 0; c < 3; ++c)
                if (r12Profile.colorFeedbackPlanes & (c ? 2 : 1))
                    for (size_t at = 0; at < planes[c].size(); ++at)
                        planes[c][at] += r12Profile.colorFeedback * (planes[c][at] - decoded[c][at]);
            timing.mark("encode.perceptual_color_feedback");
        }
        if (!cflMap.empty()) {
            const uint32_t cols = (f.width + cflTile - 1) / cflTile, rows = (f.height + cflTile - 1) / cflTile;
            require(cflMap.size() == size_t(cols) * rows * 2 && cflMeans.size() == size_t(cols) * rows,
                    "Invalid R6 encode model.");
            const std::span<const float> predictorLuma(planes[0]);
            if (!o.chroma420) {
                parallelFor(uint32_t(ch), o.threads, [&](uint32_t y) {
                    thread_local std::array<std::vector<float>, 3> modelRows;
                    for (auto& v : modelRows)
                        v.resize(cw);
                    r6ModelRow(cflMap, cflMeans, cflTile, cols, rows, float(std::min(f.height - 1, y)), uint32_t(cw), 1,
                               0, modelRows[0].data(), modelRows[1].data(), modelRows[2].data());
                    int x = 0;
                    for (; x + 4 <= cw; x += 4) {
                        const auto ac = _mm_sub_ps(
                            _mm_loadu_ps(predictorLuma.data() + size_t(std::min(uint32_t(y), f.height - 1)) * w + x),
                            _mm_loadu_ps(modelRows[2].data() + x));
                        for (int c = 0; c < 2; ++c) {
                            auto* dst = planes[c + 1].data() + size_t(y) * cw + x;
                            _mm_storeu_ps(dst, _mm_sub_ps(_mm_loadu_ps(dst),
                                                          _mm_mul_ps(_mm_loadu_ps(modelRows[c].data() + x), ac)));
                        }
                    }
                });
            } else
                parallelFor(uint32_t(ch), o.threads, [&](uint32_t y) {
                    thread_local std::array<std::vector<float>, 3> modelRows;
                    for (auto& v : modelRows)
                        v.resize(cw);
                    r6ModelRow(cflMap, cflMeans, cflTile, cols, rows, float(std::min(f.height - 1, y * 2)) + .5f,
                               uint32_t(cw), 2, .5f, modelRows[0].data(), modelRows[1].data(), modelRows[2].data());
                    const uint32_t sy = std::min(f.height - 1, y * 2), ny = std::min(f.height - 1, sy + 1);
                    for (int x = 0; x < cw; ++x) {
                        const uint32_t sx = std::min(f.width - 1, uint32_t(x * 2)), nx = std::min(f.width - 1, sx + 1);
                        const float ac = ((predictorLuma[size_t(sy) * w + sx] + predictorLuma[size_t(sy) * w + nx]) +
                                          (predictorLuma[size_t(ny) * w + sx] + predictorLuma[size_t(ny) * w + nx])) *
                                             .25f -
                                         modelRows[2][x];
                        for (int c = 0; c < 2; ++c)
                            planes[c + 1][size_t(y) * cw + x] -= modelRows[c][x] * ac;
                    }
                });
        }
    }
    timing.mark(gpuPrepared?"encode.color_forward.gpu":reuseSource ? "encode.reuse_pilot_source" : "encode.color_conversion");
    // At low rates the extra chroma detail competes with essential luminance.
    if (!reuseSource && !r5 && !(tuned && restoration && 3004u < 10) && true && o.chroma420 && allowFlatRegions &&
        o.targetBytes && uint64_t(o.targetBytes) * 1000 >= uint64_t(f.width) * f.height * 35)
        for (int c = 1; c < 3; ++c)
            precompensateChroma(planes[c], cw, ch, o.threads);
    Bytes flatMetadata, encodedFlatMetadata;
    if (allowFlatRegions && spatialCoding && 3004u < 1800) {
        flatMetadata =
            flatRegions(f, uint64_t(o.targetBytes) * 1000 >= uint64_t(f.width) * f.height * 30 ? 8 : 16, o.threads);
        if (!flatMetadata.empty()) {
            encodedFlatMetadata = entropyEncode(flatMetadata);
            const size_t offset = 2 + size_t(flatMetadata[1]) * 4, cells = flatMetadata.size() - offset;
            const auto covered = std::count_if(flatMetadata.begin() + offset, flatMetadata.end(),
                                               [](uint8_t index) { return index != 0; });
            if (uint64_t(encodedFlatMetadata.size()) * cells * 10 > uint64_t(o.targetBytes) * covered) {
                flatMetadata.clear();
                encodedFlatMetadata.clear();
            }
        }
    }
    timing.mark("encode.precomp_flat_analysis");
    if (r12Profile.cflPilotGain && !cflMap.empty()) {
        require(reuseSource && o.chroma420 && synthesisDecoded.size() == planes[0].size(),
                "Invalid reconstructed-luma chroma correction.");
        const uint32_t cols = (f.width + cflTile - 1) / cflTile, rows = (f.height + cflTile - 1) / cflTile;
        std::vector<float> decodedMeans(size_t(cols) * rows);
        parallelFor(rows, o.threads, [&](uint32_t ty) {
            for (uint32_t tx = 0; tx < cols; ++tx) {
                double sum = 0;
                uint32_t count = 0;
                for (uint32_t y = ty * cflTile; y < std::min(f.height, (ty + 1) * cflTile); ++y)
                    for (uint32_t x = tx * cflTile; x < std::min(f.width, (tx + 1) * cflTile); ++x) {
                        sum += synthesisDecoded[size_t(y) * w + x];
                        ++count;
                    }
                decodedMeans[size_t(ty) * cols + tx] = float(sum / count);
            }
        });
        std::array<std::vector<float>, 2> correction;
        for (auto& p : correction)
            p.resize(size_t(cw) * ch);
        parallelFor(uint32_t(ch), o.threads, [&](uint32_t y) {
            thread_local std::array<std::vector<float>, 6> modelRows;
            for (auto& p : modelRows)
                p.resize(cw);
            const float modelY = float(std::min(f.height - 1, y * 2)) + .5f;
            r6ModelRow(cflMap, cflMeans, cflTile, cols, rows, modelY, uint32_t(cw), 2, .5f, modelRows[0].data(),
                       modelRows[1].data(), modelRows[2].data());
            r6ModelRow(cflMap, decodedMeans, cflTile, cols, rows, modelY, uint32_t(cw), 2, .5f, modelRows[3].data(),
                       modelRows[4].data(), modelRows[5].data());
            const uint32_t sy = std::min(f.height - 1, y * 2), ny = std::min(f.height - 1, sy + 1);
            for (int x = 0; x < cw; ++x) {
                const uint32_t sx = std::min(f.width - 1, uint32_t(x * 2)), nx = std::min(f.width - 1, sx + 1);
                auto ac = [&](const std::vector<float>& p, float mean) {
                    return ((p[size_t(sy) * w + sx] + p[size_t(sy) * w + nx]) +
                            (p[size_t(ny) * w + sx] + p[size_t(ny) * w + nx])) *
                               .25f -
                           mean;
                };
                const float difference =
                    ac(synthesisSource.spatialLuma, modelRows[2][x]) - ac(synthesisDecoded, modelRows[5][x]);
                for (int c = 0; c < 2; ++c)
                    correction[c][size_t(y) * cw + x] = r12Profile.cflPilotGain * modelRows[c][x] * difference;
            }
        });
        for (int c = 0; c < 2; ++c) {
            transformNativePlane(correction[c], cw, ch, o.threads, false,
                                 nativeKernelForChannel(alternativeKernel, c + 1));
            for (size_t i = 0; i < correction[c].size(); ++i)
                planes[c + 1][i] += correction[c][i];
        }
        timing.mark("encode.reconstructed_luma_chroma_correction");
    }
    if (r12Profile.synthesisGain && !r12Profile.approximatePilot) {
        require(synthesisDecoded.size() == planes[0].size(), "Invalid synthesis pilot luma size.");
        synthesisGradient.resize(planes[0].size());
        const auto& spatialLuma = reuseSource ? synthesisSource.spatialLuma : planes[0];
        require(spatialLuma.size() == planes[0].size(), "Invalid cached synthesis source luma size.");
        parallelFor(uint32_t(h), o.threads, [&](uint32_t y) {
            for (int x = 0; x < w; ++x) {
                const auto at = size_t(y) * w + x;
                synthesisGradient[at] = spatialLuma[at] - synthesisDecoded[at];
            }
        });
        if (r12Profile.contrastLoss) {
            contrast = fitContrastLoss(spatialLuma, w, h, o.threads, uint8_t((r12Profile.contrastLoss - 1) % 3));
            for (size_t i = 0; i < synthesisGradient.size(); ++i)
                synthesisGradient[i] *= contrast.weights[i];
        }
        waveletSynthesisAdjoint(synthesisGradient, w, h, o.threads);
        waveletTransform(synthesisDecoded, w, h, o.threads, false);
    }
    if (sourceOutput)
        sourceOutput->spatialLuma = planes[0];
    {
        TimingTimeline sourceForward;
        if (!reuseSource&&!gpuPrepared)
            for (int c = 0; c < 3; ++c)
                transformNativePlane(planes[c], c ? cw : w, c ? ch : h, o.threads, false,
                                     nativeKernelForChannel(alternativeKernel, c));
        sourceForward.mark("encode.detail.source_forward_wavelet");
    }
    if (r12Profile.chromaDetailThreshold) {
        require(!o.chroma420 && alternativeKernel == 10, "Adaptive chroma needs full-resolution CDF 9/7.");
        omitSmoothChromaDetails(f, planes, w, h, r12Profile.chromaDetailThreshold, o.threads);
        timing.mark("encode.adaptive_chroma_bands");
    }
    const bool quantAvx2 = hasAvx2();
    const uint32_t rateTarget = o.targetBytes;
    const float qualityStep = float(110 / std::pow(q + 2., .70));
    float step = qualityStep;
    std::array<uint64_t, 8193> suffixCount{}, suffixBits{};
    auto estimate = [&](float value) {
        const double offset = std::log2(4. * value);
        const int first = std::clamp(int(value * .65f * 4), 1, 8192),
                  active = std::max(first, std::min(8192, 1 << std::max(0, int(std::floor(offset)))));
        return 200. + (4. * suffixCount[first] +
                       .7 * std::max(0., double(suffixBits[active]) - offset * suffixCount[active])) /
                          8.;
    };
    auto findStep = [&](double wanted) {
        float lo = .0625f, hi = 2048;
        for (int i = 0; i < 15; ++i) {
            const float middle = (lo + hi) * .5f;
            if (estimate(middle) > wanted)
                lo = middle;
            else
                hi = middle;
        }
        return std::max(qualityStep, hi);
    };
    auto histogramStep = [&] {
        // Identical estimator for both the non-stream pilot and final packets.
        // Rebuild after synthesis correction so the final rate uses its data.
        suffixCount.fill(0);
        suffixBits.fill(0);
        std::array<std::array<uint32_t, 8192>, 3> histograms{};
        parallelFor(3, o.threads, [&](uint32_t c) {
            const int pw = c ? cw : w, ph = c ? ch : h;
            bands(pw, ph, [&](int level, int band, int bx, int by, int bw, int bh) {
                const float bandScale = legacyRate ? quantStep(1, level, band, detail)
                                                   : (band == 2 ? diagonal : 1.f) * std::pow(slope, float(level));
                const float inverse = 1 / (bandScale * (c ? chromaScale : 1.f));
                for (int y = by; y < by + bh; y += 2) {
                    const auto* row = planes[c].data() + size_t(y) * pw + bx;
                    const auto rows = std::min(2, by + bh - y);
                    if (quantAvx2)
                        waveletHistogramAvx2(row, bw, inverse, rows, histograms[c].data());
                    else
                        for (int x = 0; x < bw; x += 4) {
                            int bin = std::min(8191, int(std::abs(row[x]) * inverse * 4));
                            if (bin)
                                histograms[c][bin] += rows * std::min(4, bw - x);
                        }
                }
            });
        });
        std::array<uint32_t, 8192> histogram{};
        for (int bin = 0; bin < 8192; ++bin)
            for (const auto& hist : histograms)
                histogram[bin] += hist[bin];
        for (int bin = 8191; bin >= 0; --bin) {
            suffixCount[bin] = suffixCount[bin + 1] + histogram[bin];
            suffixBits[bin] = suffixBits[bin + 1] + uint64_t(histogram[bin]) * std::bit_width(unsigned(bin));
        }
        return findStep(rateTarget / (legacyRate ? 1. : detail ? 1.25 : spatialCoding ? 1.03 : 1.13));
    };
    if (r12Profile.approximatePilot && !decodedPilot) {
        // These detail intervals are children of approximate_synthesis. Keep
        // the historical inclusive interval for existing diagnostic readers.
        TimingTimeline pilotDetail;
        require(r12Profile.approximatePilot <= 2 && alternativeKernel == 10 && r12Profile.synthesisGain &&
                    !r12Profile.colorFeedback && !r12Profile.contrastLoss && !reuseSource,
                "Invalid approximate synthesis profile.");
        const float guess = rateTarget ? histogramStep() : qualityStep;
        pilotDetail.mark("encode.detail.pilot_rate_estimate");
        ApproximatePilotOptions pilotOptions;
        pilotOptions.baseStep = std::clamp(std::round(guess * r12Profile.pilotStepScale * 16) / 16, .0625f, 4095.f);
        pilotOptions.slope = slope;
        pilotOptions.diagonal = diagonal;
        pilotOptions.rounding = r12Profile.rounding;
        pilotOptions.adaptiveRate = r12Profile.adaptiveRate;
        pilotOptions.adaptive = uint8_t(r12Profile.approximatePilot - 1);
        auto pilot = approximateLumaPilot(planes[0], uint32_t(w), uint32_t(h), o.threads, pilotOptions, false);
        synthesisDecoded = std::move(pilot.coefficients);
        synthesisGradient.resize(planes[0].size());
        pilotDetail.mark("encode.detail.pilot_quantization");
        parallelFor(uint32_t(h), o.threads, [&](uint32_t y) {
            for (int x = 0; x < w; ++x) {
                const auto at = size_t(y) * w + x;
                synthesisGradient[at] = planes[0][at] - synthesisDecoded[at];
            }
        });
        waveletTransform(synthesisGradient, w, h, o.threads, true);
        waveletSynthesisAdjoint(synthesisGradient, w, h, o.threads);
        pilotDetail.mark("encode.detail.pilot_gradient");
        timing.mark("encode.approximate_synthesis");
    }
    if (r12Profile.synthesisGain && !decodedPilot) {
        const auto& energies = fineSynthesisEnergies();
        parallelFineBands(w, h, o.threads, [&](int level, int band, int bx, int by, int bw, int row) {
            const float inverseEnergy = 1 / energies[level * 3 + band];
            for (int x = bx; x < bx + bw; ++x) {
                const auto at = size_t(by + row) * w + x;
                const float error = planes[0][at] - synthesisDecoded[at];
                planes[0][at] += r12Profile.synthesisGain *
                                 (synthesisGradient[at] * inverseEnergy /
                                      (r12Profile.contrastLoss >= 4
                                           ? contrast.basisWeight((x - bx) << (level + 1),
                                                                  int(by + row - by) << (level + 1), 5 << (level + 1))
                                           : 1.f) -
                                  error);
            }
        });
        timing.mark("encode.synthesis_error_correction");
    }

    std::vector<float> decodedPilotSource;
    if(decodedPilot)decodedPilotSource=planes[0];
    if (r12Profile.middleBandGain) {
        bands(w, h, [&](int level, int, int bx, int by, int bw, int bh) {
            if (level != 1 && level != 2)
                return;
            parallelFor(uint32_t(bh), o.threads, [&](uint32_t row) {
                for (int x = bx; x < bx + bw; ++x)
                    planes[0][size_t(by + row) * w + x] *= 1 + r12Profile.middleBandGain;
            });
        });
    }
    timing.mark("encode.forward_wavelet");
    bool opaque = true;
    const auto mask = _mm_set1_epi32(int(0xff000000u));
    size_t at = 0;
    if (reuseSource)
        opaque = synthesisSource.opaque;
    else if (knownOpaqueSource)
        opaque = true;
    else if (hasAvx2())
        opaque = waveletOpaqueAvx2(f.pixels.data(), f.pixels.size());
    else {
        for (; at + 16 <= f.pixels.size(); at += 16) {
            auto v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(f.pixels.data() + at));
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(v, mask), mask)) != 65535) {
                opaque = false;
                break;
            }
        }
        if (opaque)
            for (; at < f.pixels.size(); at += 4)
                if (f.pixels[at + 3] != 255) {
                    opaque = false;
                    break;
                }
    }
    Bytes alpha;
    if (reuseSource)
        alpha = std::move(synthesisSource.alpha);
    else if (!opaque)
        alpha = predictive(f, 3);
    timing.mark("encode.alpha_analysis");
    std::unique_ptr<SourcePriority> ownedPriority;
    SourcePriority* sourcePriority = nullptr;
    std::array<std::vector<float>, 216> priorityRates, budgetCosts, budgetPrecisions, budgetCoefficients;
    bool captureBudget = false;
    if (r12Profile.sourcePriority) {
        if (synthesisSource.importance && synthesisSource.importanceMode == r12Profile.sourcePriority) {
            sourcePriority = synthesisSource.importance.get();
            timing.mark("encode.source_priority_reuse");
        } else {
            ownedPriority = std::make_unique<SourcePriority>(f, o.threads, r12Profile.sourcePriority,
                                                            o.adaptiveAllocationControls && o.flatAllocation > 50);
            sourcePriority = ownedPriority.get();
            timing.mark("encode.source_priority_analysis");
        }
        if (sourceOutput && ownedPriority) {
            sourceOutput->importance = std::move(ownedPriority);
            sourceOutput->importanceMode = r12Profile.sourcePriority;
        }
    }
    const float geometryAllocationGain = std::exp2(-float(o.flatAllocation) * .02f *
                                                   (float(o.adaptiveAllocationStrength) / 100.f));
    const float fullFlatAllocationRate = adaptiveAllocationRate(1.f, 1.f, false, o);
    auto allocationSourceRate = [&](uint32_t x, uint32_t y, uint32_t fw, uint32_t fh, bool chroma, bool fineGroup = false) {
        float factor = sourcePriority->bandRate(x, y, fw, fh);
        const uint32_t span = std::max(fw, fh);
        float coverage = span <= 256 ? sourcePriority->flatCoverage(x, y, fw, fh, o.flatAllocation) : 0.f;
        // The scale-dependent wavelet halo can encompass an entire smooth
        // wall in coarse bands. Extra savings may reclaim its verified flat
        // interior, but preserve every recognized edge and a fixed source-cell
        // margin. Fine glyph support is unchanged; only the distant halo loses
        // its unconditional edge classification.
        if ((fineGroup || o.flatThreshold) && (o.flatSavings || o.flatThreshold) &&
            coverage >= flatMinimumCoverage(o.flatThreshold)) {
            // 50% keeps one full classifier-cell margin; 100% still keeps
            // half a cell. The control can therefore reclaim more interior
            // than a saturated coefficient price alone would permit.
            const uint32_t margin = 12 - std::min(100u, o.flatSavings) * 8 / 100;
            if (!sourcePriority->hasEdgeWithMargin(x,y,fw,fh,margin)) {
                if (factor < 1) factor = 1.f;
                if (o.flatThreshold) coverage = 1.f;
            }
        }
        // A coefficient also affects the adjacent wavelet support. Fine
        // groups keep the existing half-footprint halo; checking only their
        // center footprint erased tiny glyphs next to a nominally flat cell.
        const bool geometry = factor < 1 && (fineGroup || sourcePriority->hasDirectEdge(x, y, fw, fh));
        if (geometry) coverage = 0;
        else if (fineGroup && o.flatAllocation > 50)
            coverage = allocationFlatCoverage(coverage, o.flatAllocation);
        // Exact endpoints avoid log/exp work for the common fully protected
        // and fully flat cases. They have the same chosen RDO weights.
        const bool maximumContrast = fineGroup && o.flatAllocation == 100 && o.adaptiveAllocationStrength >= 100;
        if (maximumContrast && geometry) return .03125f;
        if (maximumContrast) return 128.f;
        if (coverage == 1 && factor == 1 && (!fineGroup || o.flatAllocation <= 50)) return fullFlatAllocationRate;
        factor = adaptiveAllocationRate(factor, coverage, chroma, o);
        if (geometry && o.adaptiveAllocationStrength && span <= 256)
            factor = std::max(.03125f, factor * geometryAllocationGain);
        // The upper half of the slider can deliberately approach two
        // allocation classes, but only on fine RDO groups with the full
        // geometric support halo. Never saturate mixed coarse quantizers.
        if (fineGroup && o.flatAllocation > 50 && o.adaptiveAllocationStrength) {
            const float amount = (float(o.flatAllocation) - 50.f) / 50.f *
                                 std::min(1.f, float(o.adaptiveAllocationStrength) / 100.f);
            if (geometry)
                factor = std::exp2(std::log2(factor) * (1 - amount) - 5.f * amount);
            else if (coverage > 0)
                factor = std::exp2(std::log2(factor) * (1 - amount * coverage) + 7.f * amount * coverage);
        }
        return std::clamp(factor, .03125f, 128.f);
    };
    // Four-coefficient RDO groups match the source cell much more closely
    // than 16-coefficient entropy tiles. Reuse one grid across orientations
    // and both chroma axes; never encode another map or change the decoder.
    const float flatRateBoost = o.adaptiveQuantizationEnabled ? std::exp2(std::min(12.f, float(o.flatSavings) * .06f)) : 1.f;
    const uint32_t groupLevels = o.flatSavings ? 6u : 4u;
    auto rateCell = [&](int level) {return o.flatSavings ? std::max(1, 4 >> std::min(level,2)) : 4;};
    std::array<std::vector<float>, 12> groupPriorityRates;
    if (sourcePriority && lowRate && o.adaptiveAllocationControls && (r12Profile.adaptive == 0 || r12Profile.adaptive == 3) && r12Profile.quantRdo) {
        // Fine luma groups dominate the work. One job per band left most
        // workers idle while that largest grid finished; split independent
        // rows instead, without changing a cell's computation or rounding.
        struct RateRows { uint32_t grid; int first, end; };
        std::vector<RateRows> jobs;
        for(uint32_t job=0;job<2*groupLevels;++job){
            const uint32_t c=job/groupLevels,level=job%groupLevels;
            const int bw=(c?cw:w)>>(level+1),bh=(c?ch:h)>>(level+1),cell=rateCell(level),cols=(bw+cell-1)/cell;
            groupPriorityRates[c*6+level].resize(size_t(cols)*((bh+cell-1)/cell));
            for(int y=0;y<bh;y+=cell*4)jobs.push_back({job,y,std::min(bh,y+cell*4)});
        }
        parallelFor(uint32_t(jobs.size()), o.threads, [&](uint32_t task) {
            const auto& rows=jobs[task];const uint32_t job=rows.grid;
            const uint32_t c = job / groupLevels, level = job % groupLevels;
            const int bw = (c ? cw : w) >> (level + 1), bh = (c ? ch : h) >> (level + 1), cell = rateCell(level), cols = (bw + cell - 1) / cell;
            const uint32_t sy = (1u << (level + 1)) * (c && o.chroma420 ? 2u : 1u),
                           sx = sy * (c && o.chroma422 ? 2u : 1u);
            auto& rates = groupPriorityRates[c * 6 + level];
            for (int y = rows.first; y < rows.end; y += cell) for (int x = 0; x < bw; x += cell)
                rates[size_t(y / cell) * cols + x / cell] = allocationSourceRate(x * sx, y * sy,
                    std::min(cell, bw - x) * sx, std::min(cell, bh - y) * sy, c != 0, true);
        });
        timing.mark("encode.source_group_priority");
    }
    std::array<std::vector<SpatialTileBound>, 216> cachedBounds;
    const bool detailedSpatialTiming=timingEvents&&std::getenv("FAJK_PROFILE_SPATIAL");
    bool captureActualPilot = decodedPilot;
    if(captureActualPilot)synthesisDecoded.assign(planes[0].size(),0.f);
    auto pack = [&](float requestedStep) {
        TimingTimeline packTiming;
        const auto fixed = uint16_t(std::clamp(int(std::round(requestedStep * 16)), 1, 65535));
        const float baseStep = fixed / 16.f;
        std::array<ByteWriter, 17> streams;
        std::array<std::vector<uint32_t>, 17> partitions;
        std::array<Bytes, 3> significance;
        if (r12Profile.parent)
            for (int c = 0; c < 3; ++c)
                significance[c].assign(planes[c].size(), 0);
        struct Band {
            ByteWriter runs, values;
            bool shortMagnitude = false;
        };
        std::array<Band, 216> packed;
        std::array<SpatialWorkProfile,216> spatialProfiles;
        auto packBand = [&](uint32_t job) {
            SpatialWorkCapture spatialCapture(detailedSpatialTiming?&spatialProfiles[job]:nullptr);
            const int index = partitionedSpatial ? job / 4 : job, c = index / 18, level = (index % 18) / 3,
                      band = index % 3;
            if(captureActualPilot&&c)return;
            const int pw = c ? cw : w, ph = c ? ch : h, rw = pw >> level, rh = ph >> level, bw = rw / 2, bh = rh / 2,
                      bx = band == 1 ? 0 : bw, by = band == 0 ? 0 : bh;
            const float step = quantStep(baseStep, level, band, detail) * (c ? chromaScale : 1.f);
            uint32_t zeros = 0;
            auto& out = packed[spatialCoding && !partitionedSpatial ? job * 4 : job];
            if (spatialCoding) {
                const int rows = partitionedSpatial ? spatialRows(bw, bh, r5 ? r12Profile.spatialParts : 4) : bh,
                          offset = partitionedSpatial ? int(job % 4) * rows : 0;
                if (offset >= bh)
                    return;
                auto& rates = priorityRates[spatialCoding && !partitionedSpatial ? job * 4 : job];
                auto& bounds = cachedBounds[spatialCoding && !partitionedSpatial ? job * 4 : job];
                if ((r12Profile.adaptive == 0 || r12Profile.adaptive == 3) && bounds.empty())
                    bounds = spatialTileBounds(planes[c].data() + size_t(by + offset) * pw + bx, pw, bw,
                                               std::min(rows, bh - offset), r12Profile.entropyMode);
                const bool priorityLumaOnly = r12Profile.sourcePriority == 18;
                if (sourcePriority && (!priorityLumaOnly || !c) && rates.empty()) {
                    const int localHeight = std::min(rows, bh - offset),
                              extent = spatialTileExtent(r12Profile.entropyMode), cols = (bw + extent - 1) / extent,
                              tileRows = (localHeight + extent - 1) / extent;
                    rates.resize(size_t(cols) * tileRows);
                    const uint32_t scaleY = uint32_t(1u << (level + 1)) * (c && o.chroma420 ? 2u : 1u),
                                   scale = scaleY * (c && o.chroma422 ? 2u : 1u);
                    for (int ty = 0; ty < localHeight; ty += extent)
                        for (int tx = 0; tx < bw; tx += extent) {
                            float factor;
                            if (o.adaptiveAllocationControls) {
                                factor = allocationSourceRate(uint32_t(tx) * scale, uint32_t(offset + ty) * scaleY,
                                    uint32_t(std::min(extent, bw - tx)) * scale,
                                    uint32_t(std::min(extent, localHeight - ty)) * scaleY, c != 0, r12Profile.entropyMode == 32);
                            } else {
                                factor = sourcePriority->bandRate(uint32_t(tx) * scale, uint32_t(offset + ty) * scaleY,
                                    uint32_t(std::min(extent, bw - tx)) * scale,
                                    uint32_t(std::min(extent, localHeight - ty)) * scaleY);
                                if (o.adaptiveQuantizationMode == 3 && uint32_t(extent) * scale <= 64) {
                                    const float coverage = sourcePriority->flatCoverage(
                                        uint32_t(tx) * scale, uint32_t(offset + ty) * scaleY,
                                        uint32_t(std::min(extent, bw - tx)) * scale,
                                        uint32_t(std::min(extent, localHeight - ty)) * scaleY);
                                    factor = std::min(factor, 1 - .75f * coverage);
                                }
                                const auto strength =
                                    o.adaptiveQuantizationMode == 4 && !c ? 100u : o.adaptiveQuantization;
                                if (strength != 100) {
                                    const float originalFactor = factor;
                                    factor = std::clamp(std::pow(factor, float(strength) / 100.f),
                                                        o.adaptiveQuantizationMode ? .03125f : .25f, 128.f);
                                    if (o.adaptiveQuantizationMode >= 2 && originalFactor > 1)
                                        factor = std::min(factor, originalFactor);
                                }
                            }
                            rates[size_t(ty / extent) * cols + tx / extent] = factor;
                        }
                }
                const bool compact = lowRate && o.waveletStage == WaveletStage::Final && r12Profile.adaptive == 0;
                thread_local std::vector<float> cachedIntegers;
                std::vector<float> largeIntegers;
                auto& selectedIntegers =
                    size_t(bw) * std::min(rows, bh - offset) <= 1024 * 1024 ? cachedIntegers : largeIntegers;
                auto coded = encodeSpatialBand(
                    planes[c].data() + size_t(by + offset) * pw + bx, pw, bw, std::min(rows, bh - offset), step,
                    spatialPrediction && level >= predictionLevel,
                    r12Profile.rounding, r12Profile.median, level < 3 ? r12Profile.rdo : 0.f,
                    true, r12Profile.adaptive, r12Profile.enhanced, r12Profile.extended,
                    r12Profile.adaptiveRate,
                    r12Profile.parent && level + 1 < levels
                        ? significance[c].data() + size_t(band == 0 ? 0 : bh / 2) * pw + (band == 1 ? 0 : bw / 2)
                        : nullptr,
                    pw, r12Profile.parent ? significance[c].data() + size_t(by + offset) * pw + bx : nullptr, pw,
                    r12Profile.entropyMode, r12Profile.quantRdo, r12Profile.quantGradient,
                    level < 4 ? r12Profile.quantCoupling : 0.f,
                    r12Profile.quantCoupling && level < 4 ? fineSynthesisCorrelations()[level * 3 + band].data()
                                                          : nullptr,
                    level < 4 ? r12Profile.coupledQuantizer : 0, r12Profile.rateQuantizer && level < 4,
                    o.patternSimilarity, rates, r12Profile.entropyMode == 32 ? (level < 4 ? 1 : 2) : 0, bounds,
                    captureBudget ? &budgetCosts[spatialCoding && !partitionedSpatial ? job * 4 : job] : nullptr,
                    (compact || captureActualPilot || captureBudget) ? &selectedIntegers : nullptr, captureActualPilot,
                    level < int(groupLevels) ? std::span<const float>(groupPriorityRates[(c ? 1 : 0) * 6 + level]) : std::span<const float>{},
                    captureBudget ? &budgetPrecisions[spatialCoding && !partitionedSpatial ? job * 4 : job] : nullptr, flatRateBoost, rateCell(level));
                if(captureActualPilot&&!c){
                    for(int y=0;y<std::min(rows,bh-offset);++y)
                        for(int x=0;x<bw;++x)
                            synthesisDecoded[size_t(by+offset+y)*pw+bx+x]=selectedIntegers[size_t(y)*bw+x]*step;
                }
                if (compact && !captureActualPilot && !coded.empty()) {
                    // Recode the already selected integers, never requantize the
                    // source. Both candidates reconstruct exactly the same band.
                    coded = compactSpatialBand(
                        coded, bw, std::min(rows, bh - offset), spatialPrediction && level >= predictionLevel,
                        captureBudget ? &budgetCosts[spatialCoding && !partitionedSpatial ? job * 4 : job] : nullptr,
                        selectedIntegers);
                }
                if (captureBudget)
                    budgetCoefficients[spatialCoding && !partitionedSpatial ? job * 4 : job] = selectedIntegers;
                out.shortMagnitude = !coded.empty() && coded.front() != 4;
                if (r5)
                    out.values.var(uint32_t(coded.size()));
                else
                    out.values.u32(uint32_t(coded.size()));
                out.values.bytes(coded);
                return;
            }
            const auto inverse = _mm_set1_ps(1 / step), sign = _mm_set1_ps(-0.f), bias = _mm_set1_ps(.35f);
            auto emit = [&](int value) {
                if (!value) {
                    ++zeros;
                    return;
                }
                out.runs.var(zeros + 1);
                zeros = 0;
                out.values.signedVar(value);
            };
            const int tile = detail ? 16 : std::max(bw, bh);
            for (int ty = by; ty < by + bh; ty += tile)
                for (int tx = bx; tx < bx + bw; tx += tile)
                    for (int y = ty; y < std::min(by + bh, ty + tile); ++y) {
                        int x = tx;
                        const float* data = planes[c].data() + size_t(y) * pw;
                        const int end = std::min(bx + bw, tx + tile);
                        if (quantAvx2)
                            for (; x + 16 <= end; x += 16) {
                                alignas(32) int values[16];
                                if (!waveletQuantize16Avx2(data + x, 1 / step, values)) {
                                    zeros += 16;
                                    continue;
                                }
                                for (int value : values)
                                    emit(value);
                            }
                        for (; x + 4 <= end; x += 4) {
                            const auto v = _mm_mul_ps(_mm_loadu_ps(data + x), inverse);
                            const auto magnitude = _mm_cvttps_epi32(_mm_add_ps(_mm_andnot_ps(sign, v), bias));
                            if (_mm_movemask_epi8(_mm_cmpeq_epi32(magnitude, _mm_setzero_si128())) == 65535) {
                                zeros += 4;
                                continue;
                            }
                            alignas(16) int values[4];
                            _mm_store_si128(reinterpret_cast<__m128i*>(values), magnitude);
                            const int negative = _mm_movemask_ps(v);
                            for (int i = 0; i < 4; ++i)
                                emit((negative & (1 << i)) ? -values[i] : values[i]);
                        }
                        for (; x < end; ++x) {
                            const float v = data[x] / step;
                            const int magnitude = int(std::abs(v) + .35f);
                            emit(v < 0 ? -magnitude : magnitude);
                        }
                    }
            out.runs.var(0);
        };
        if (r12Profile.parent) {
            for (int level = levels - 1; level >= 0; --level)
                parallelFor(9, o.threads, [&](uint32_t job) { packBand((job / 3) * 18 + level * 3 + job % 3); });
        } else
            parallelFor(partitionedSpatial ? 216 : 54, o.threads, packBand);
        if(detailedSpatialTiming){
            constexpr const char* names[]{"encode.work.spatial.prepare","encode.work.spatial.precision",
                "encode.work.spatial.quantization","encode.work.spatial.rdo","encode.work.spatial.coding"};
            for(unsigned part=0;part<5;++part){double sum=0;for(const auto& p:spatialProfiles)sum+=p.ms[part];timingEvents->push_back({names[part],sum});}
        }
        for (int c = 0; c < 3; ++c) {
            if(captureActualPilot&&c)continue;
            const int pw = c ? cw : w, ph = c ? ch : h, base = 1 + c * 5;
            const float dcStep = quantStep(baseStep, 0, -1, detail) * (c ? chromaScale : 1.f);
            const int lw = pw >> levels, lh = ph >> levels;
            std::vector<int> previous(lw);
            int corner = 0;
            for (int y = 0; y < lh; ++y) {
                int left = 0;
                corner = 0;
                for (int x = 0; x < lw; ++x) {
                    const int value = int(std::round(planes[c][size_t(y) * pw + x] / dcStep)), top = previous[x];
                    if(captureActualPilot)synthesisDecoded[size_t(y)*pw+x]=float(value)*dcStep;
                    streams[base].signedVar(value - medianPredict(left, top, corner));
                    previous[x] = value;
                    corner = top;
                    left = value;
                }
            }
            if(captureActualPilot)continue;
            for (int level = 0; level < levels; ++level)
                for (int band = 0; band < 3; ++band) {
                    if (spatialCoding) {
                        for (int part = 0; part < 4; ++part)
                            streams[base + 2 + band].bytes(packed[(c * 18 + level * 3 + band) * 4 + part].values.data);
                        continue;
                    }
                    auto& data = packed[c * 18 + level * 3 + band];
                    streams[base + 1].bytes(data.runs.data);
                    streams[base + 2 + band].bytes(data.values.data);
                    partitions[base + 1].push_back(uint32_t(streams[base + 1].data.size()));
                    partitions[base + 2 + band].push_back(uint32_t(streams[base + 2 + band].data.size()));
                }
        }
        packTiming.mark("encode.pack_quantization_range_coding");
        if(captureActualPilot)return Packet{{},f.pts,f.duration};
        streams[16].data = alpha;
        std::array<Bytes, 17> coded;
        std::array<std::array<Bytes, 6>, 17> bandCoded;
        // Shared models and individual band models are independent work. Do
        // not serialize six entropy passes behind one large luma stream.
        parallelFor(spatialCoding ? 17 : 71, o.threads, [&](uint32_t job) {
            if (job < 17) {
                if (nativeR6 && compactNative && streams[job].data.empty())
                    coded[job].clear();
                else
                    coded[job] = job == 0 && !flatMetadata.empty()
                                     ? encodedFlatMetadata
                                     : entropyEncode(streams[job].data, 3004u >= 2320 && 3004u <= 2348);
                return;
            }
            const int bandJob = job - 17, c = bandJob / 18, level = (bandJob % 18) / 3, band = bandJob % 3,
                      i = 3 + c * 5 + band;
            if (streams[i].data.size() >= 4096)
                bandCoded[i][level] = entropyEncode(packed[bandJob].values.data);
        });
        if (!spatialCoding)
            parallelFor(9, o.threads, [&](uint32_t job) {
                const int i = 3 + (job / 3) * 5 + job % 3;
                coded[i] = entropySelectPartitioned(streams[i].data, partitions[i], std::move(coded[i]), bandCoded[i]);
            });
        packTiming.mark("encode.pack_entropy");
        ByteWriter out;
        out.u32(frameMagic);
        out.u8(r12                         ? (splitLumaHeader          ? 114
                                              : r12Profile.colorSpace  ? 91
                                              : r12Profile.entropyMode ? 90
                                              : r12Profile.parent      ? 89
                                              : r12Profile.extended    ? 82
                                              : r12Profile.enhanced    ? (r12Profile.median ? 81 : 78)
                                              : r12Profile.adaptive    ? (r12Profile.adaptive == 3 ? 73 : 72)
                                              : r12Profile.rgb         ? 68
                                              : r12Profile.median      ? 67
                                                                       : 65)
               : filterPlaneSweep          ? 64
               : hybridReconstruction      ? 63
               : alternativeKernel         ? 61
               : guidedExperiment          ? 58
               : restorationExperiment     ? 57
               : predictionExperiment      ? 56
               : nativeR6 && compactNative ? 54
               : nativeR6                  ? 40
               : r5                        ? 29
               : r4                        ? 27
               : tuned                     ? 27
               : spatialPrediction         ? 20
               : !flatMetadata.empty()     ? 18
               : spatialCoding             ? 16
               : balancedChroma            ? 15
                                           : 14);
        out.u8(3);
        out.u8(uint8_t(q));
        out.u8(uint8_t((o.chroma420 ? 2 : 0) | (o.chroma422 ? 64 : 0) | (opaque ? 1 : 0) |
                       (detail && !r12Profile.noReconstructionBias && (r5 || !fastPhoto || guidedProfile) ? 4 : 0) |
                       (balancedChroma ? 8 : 0) | (tuned && spatialPrediction ? 16 : 0) | (nearest ? 32 : 0)));
        out.u32(f.width);
        out.u32(f.height);
        out.u16(fixed);
        if (tuned || r4 || r5) {
            out.u8(tuned ? slopeCode : 166);
            out.u8(tuned ? diagonalCode : 173);
            out.u8(tuned ? chromaCode : 211);
            out.u8(tuned ? restoration : 0);
        }
        if (predictionExperiment)
            out.u8(uint8_t(predictionLevel));
        if (restorationExperiment)
            out.u8(nativeStrength);
        if (guidedExperiment)
            out.u8(nativeGuide);
        if (alternativeKernel)
            out.u8(uint8_t(alternativeKernel));
        if (hybridReconstruction || r12) {
            out.u8(nativeStrength);
            out.u8(nativeGuide);
            if (filterPlaneSweep || r12)
                out.u8(r12 ? r12Profile.planes : nativeFilterPlanes);
            if (r12 && (r12Profile.median || r12Profile.extended))
                out.u8(r12Profile.prediction);
            if (r12 && r12Profile.extended)
                out.u8(r12Profile.median);
            if (r12 && r12Profile.rgb)
                out.u8(r12Profile.rgb);
            if (r12 && (r12Profile.adaptive || r12Profile.extended))
                out.u8(r12Profile.adaptive);
            if (r12Profile.entropyMode || splitLumaHeader)
                out.u8(uint8_t((std::any_of(packed.begin(), packed.end(),
                                                       [](const Band& b) { return b.shortMagnitude; })
                                    ? 31
                                    : r12Profile.entropyMode) |
                               (r12Profile.spatialParts == 2   ? 64
                                : r12Profile.spatialParts == 4 ? 128
                                                               : 0)));
            if (r12Profile.colorSpace || splitLumaHeader)
                out.u8(r12Profile.colorSpace);
            if (splitLumaHeader)
                out.u8(r12Profile.lumaStrength);
        }
        for (auto& stream : coded) {
            if (nativeR6 && compactNative) {
                const bool empty =
                    stream.size() == 5 && std::all_of(stream.begin(), stream.end(), [](uint8_t v) { return v == 0; });
                out.var(empty ? 0 : uint32_t(stream.size()));
                if (!empty)
                    out.bytes(stream);
            } else {
                out.u32(uint32_t(stream.size()));
                out.bytes(stream);
            }
        }
        return Packet{std::move(out.data), f.pts, f.duration};
    };
    if (rateTarget) {
        step = histogramStep();
        // The nearest adaptive quantizer retains more nonzero coefficients
        // than the coarse histogram model predicts. Start closer to its rate;
        // exact packet-size correction and the quality bound remain decisive.
        if (r12 && r12Profile.enhanced && !lowRate)
            step = std::clamp(step * 1.9f, qualityStep, 4095.f);
    }
    timing.mark("encode.histogram_rate_estimate");
    auto result = pack(decodedPilot ? step * r12Profile.pilotStepScale : step);
    if (decodedPilot) {
        captureActualPilot = false;
        // The second luma-only pilot uses the updated step. Preserve the
        // original target and reuse the capture buffer for its coefficient
        // error; no extra target copy or decoder pass is needed.
        for (int iteration = 0; iteration < 2; ++iteration) {
            parallelFor(uint32_t(h), o.threads, [&](uint32_t y) {
                for (int x = 0; x < w; ++x) {
                    const size_t at = size_t(y) * w + x;
                    synthesisDecoded[at] = decodedPilotSource[at] - synthesisDecoded[at];
                }
            });
            synthesisGradient = synthesisDecoded;
            waveletTransform(synthesisGradient, w, h, o.threads, true);
            waveletSynthesisAdjoint(synthesisGradient, w, h, o.threads);
            const auto& energies = fineSynthesisEnergies();
            const float gain = iteration ? .25f : r12Profile.synthesisGain;
            parallelFineBands(w, h, o.threads, [&](int level, int band, int bx, int by, int bw, int row) {
                const float inverse = 1 / energies[level * 3 + band];
                for (int x = bx; x < bx + bw; ++x) {
                    const size_t at = size_t(by + row) * w + x;
                    planes[0][at] += gain * (synthesisGradient[at] * inverse - synthesisDecoded[at]);
                }
            });
            // Every coefficient correction invalidates the cached maxima.
            for (auto& bounds : cachedBounds)
                bounds.clear();
            step = rateTarget ? histogramStep() : qualityStep;
            captureActualPilot = iteration == 0;
            result = pack(step);
            captureActualPilot = false;
        }
        timing.mark("encode.decoded_synthesis_correction");
    }
    float resultStep = step, trialStep = step, previousStep = 0;
    size_t trialBytes = result.data.size(), previousBytes = 0;
    if (rateTarget) {
        for (int correction = 0; correction < (r12Profile.ratePasses >= 0 ? r12Profile.ratePasses
                                               : r5                       ? 4
                                               : tuned && !fastPhoto      ? 5
                                                                          : 2);
             ++correction) {
            const double ratio = double(result.data.size()) / rateTarget;
            if (ratio <= 1 && (ratio >= (o.speed==EncodeSpeed::Fast ? .97
                                         : r5              ? .995
                                         : guidedProfile ? .995
                                         : spatialCoding ? .975
                                         : legacyRate    ? .97
                                         : detail        ? .985
                                                         : .997) ||
                               resultStep <= qualityStep))
                break;
            if (previousBytes && std::abs(std::log(trialStep / previousStep)) > .001) {
                const double exponent = std::clamp(
                    std::log(double(trialBytes) / previousBytes) / std::log(trialStep / previousStep), -5., -.25);
                step = float(trialStep * std::pow(double(rateTarget) * .997 / trialBytes, 1 / exponent));
            } else {
                const double margin = legacyRate
                                          ? std::clamp(.99 - .12 * (double(trialBytes) / rateTarget - 1), .85, .99)
                                      : detail ? .99
                                               : .998;
                step = findStep(estimate(trialStep) / (double(trialBytes) / rateTarget) * margin);
            }
            step = std::clamp(step, qualityStep, 4095.f);
            if (ratio <= 1 && std::round(step * 16) >= std::round(resultStep * 16))
                step = std::max(qualityStep, resultStep * .995f);
            if (std::round(step * 16) == std::round(trialStep * 16))
                break;
            auto candidate = pack(step);
            previousStep = trialStep;
            previousBytes = trialBytes;
            trialStep = step;
            trialBytes = candidate.data.size();
            if ((candidate.data.size() <= rateTarget && candidate.data.size() > result.data.size()) ||
                result.data.size() > rateTarget) {
                result = std::move(candidate);
                resultStep = step;
            }
        }
    }
    // Private internal pilots may exceed the target; only the final packet is
    // returned to callers, with the normal rate search and cap checks intact.
    if ((r12Profile.ratePasses < 0 || r12Profile.fitRateCap) && r5 && rateTarget && result.data.size() > rateTarget) {
        for (int retry = 0; retry < 6 && result.data.size() > rateTarget; ++retry) {
            resultStep = std::min(
                4095.f, resultStep * float(std::max(1.05, std::pow(double(result.data.size()) / rateTarget, .8))));
            result = pack(resultStep);
        }
        require(result.data.size() <= rateTarget,
                "R5 cannot fit this frame and its alpha channel in the requested size.");
    }
    if (o.budgetHeatmap && r12 && spatialCoding) {
        captureBudget = true;
        const auto measured = pack(resultStep);
        require(measured.data == result.data, "Budget diagnostics changed packet bytes.");
        auto& map = *o.budgetHeatmap;
        map.width = f.width;
        map.height = f.height;
        map.cell = 16;
        map.cols = (f.width + 15) / 16;
        map.rows = (f.height + 15) / 16;
        map.mappedBytes = 0;
        map.detailBytes = map.unmappedDetailBytes = 0;
        map.channelBits = {};
        map.tiles.clear();
        const size_t cells = size_t(map.cols) * map.rows;
        for (auto& p : map.penalties)
            p.assign(cells, 1.f);
        map.flatCoverage.assign(cells, 0.f);
        std::array<std::vector<float>,3> precisionCounts;
        map.allocationCols = uint32_t((w / 2 + 3) / 4);
        map.allocationRows = uint32_t((h / 2 + 3) / 4);
        map.allocationRates = groupPriorityRates[0];
        if (map.allocationRates.empty()) {
            const int extent = spatialTileExtent(r12Profile.entropyMode);
            map.allocationCols = uint32_t((w / 2 + extent - 1) / extent);
            map.allocationRows = uint32_t((h / 2 + extent - 1) / extent);
            map.allocationRates = priorityRates[0];
            if (map.allocationRates.empty())
                map.allocationRates.assign(size_t(map.allocationCols) * map.allocationRows, 1.f);
        }
        for (int c=0;c<3;++c) {
            map.localQuantSteps[c].assign(cells,0.f);
            precisionCounts[c].assign(cells,0.f);
        }
        // Read the actual per-band weights already used by the quantizer.
        // This is a finest-band decision map, not an estimate from decoded RGB.
        // All of this work lives inside the explicit budgetHeatmap branch.
        for (uint32_t y = 0; y < map.rows; ++y)
            for (uint32_t x = 0; x < map.cols; ++x) {
                const size_t at = size_t(y) * map.cols + x;
                if (sourcePriority) {
                    map.flatCoverage[at] = allocationFlatCoverage(
                        sourcePriority->flatCoverage(x * 16, y * 16, std::min(16u, f.width - x * 16),
                                                     std::min(16u, f.height - y * 16),
                                                     o.adaptiveAllocationControls ? o.flatAllocation : 50),
                        o.adaptiveAllocationControls ? o.flatAllocation : 50);
                    if (o.flatThreshold && map.flatCoverage[at]>=flatMinimumCoverage(o.flatThreshold) &&
                        !sourcePriority->hasEdgeWithMargin(x*16,y*16,std::min(16u,f.width-x*16),
                            std::min(16u,f.height-y*16),12-std::min(100u,o.flatSavings)*8/100))
                        map.flatCoverage[at]=1.f;
                }
                for (int c = 0; c < 3; ++c) {
                    const auto& rates = priorityRates[c * 18 * 4];
                    if (rates.empty())
                        continue;
                    const uint32_t extent = uint32_t(spatialTileExtent(r12Profile.entropyMode)),
                                   sx = 2 * (c && (o.chroma420 || o.chroma422) ? 2u : 1u),
                                   sy = 2 * (c && o.chroma420 ? 2u : 1u),
                                   cols = uint32_t(((c ? cw : w) / 2 + extent - 1) / extent);
                    const size_t index = size_t(y * 16 / (extent * sy)) * cols + x * 16 / (extent * sx);
                    if (index < rates.size())
                        map.penalties[c][at] = rates[index];
                    const auto& groups = groupPriorityRates[(c ? 1 : 0) * 6];
                    if (!groups.empty()) {
                        const uint32_t gx = 4 * sx, gy = 4 * sy,
                            groupCols = uint32_t(((c ? cw : w) / 2 + 3) / 4);
                        double total = 0, area = 0;
                        const uint32_t x0 = x * 16, y0 = y * 16,
                            x1 = std::min(f.width, x0 + 16), y1 = std::min(f.height, y0 + 16);
                        for (uint32_t yy = y0 / gy; yy * gy < y1; ++yy)
                            for (uint32_t xx = x0 / gx; xx * gx < x1; ++xx) {
                                const size_t j = size_t(yy) * groupCols + xx;
                                if (j < groups.size()) {
                                    const double share = (std::min(x1, (xx+1)*gx) - std::max(x0, xx*gx)) *
                                                         (std::min(y1, (yy+1)*gy) - std::max(y0, yy*gy));
                                    total += groups[j] * share; area += share;
                                }
                            }
                        if (area) map.penalties[c][at] = float(total / area);
                    }
                }
            }
        const size_t stride = size_t(map.cols) + 1;
        std::vector<double> difference(stride * (map.rows + 1));
        // Entropy bytes are exact per tile. Attribute those bytes to the
        // actual retained coefficient positions rather than uniformly painting
        // its whole (potentially 512px) rectangle. Intra-tile weights estimate
        // magnitude coding; they are NOT independent exact coefficient bytes.
        // All capture/copy/mapping work is inside the opt-in diagnostic path.
        auto mapFootprint = [&](double bits,uint32_t x,uint32_t y,uint32_t fw,uint32_t fh,int channel) {
            if (x>=f.width || y>=f.height) {map.unmappedDetailBytes+=bits/8;return;}
            const uint32_t x0=x/16,y0=y/16,x1=(std::min(f.width,x+fw)+15)/16,
                           y1=(std::min(f.height,y+fh)+15)/16;
            const double value=bits/double((x1-x0)*(y1-y0));
            difference[size_t(y0)*stride+x0]+=value;
            difference[size_t(y0)*stride+x1]-=value;
            difference[size_t(y1)*stride+x0]-=value;
            difference[size_t(y1)*stride+x1]+=value;
            map.mappedBytes+=bits/8; map.channelBits[channel]+=bits;
        };
        for (int job = 0; job < 216; ++job) {
            const auto& costs = budgetCosts[job];
            if (costs.empty())
                continue;
            const int index = job / 4, c = index / 18, level = (index % 18) / 3, pw = c ? cw : w, ph = c ? ch : h,
                      bw = (pw >> level) / 2, bh = (ph >> level) / 2;
            const int rows = partitionedSpatial ? spatialRows(bw, bh, r5 ? r12Profile.spatialParts : 4) : bh,
                      offset = partitionedSpatial ? (job % 4) * rows : 0,
                      extent = spatialTileExtent(r12Profile.entropyMode), cols = (bw + extent - 1) / extent;
            const uint32_t scaleY = uint32_t(1u << (level + 1)) * (c && o.chroma420 ? 2u : 1u),
                           scale = scaleY * (c && o.chroma422 ? 2u : 1u);
            for (size_t i = 0; i < costs.size(); ++i) {
                const uint32_t x = uint32_t(i % cols) * extent * scale,
                               y = (uint32_t(i / cols) * extent + offset) * scaleY;
                map.detailBytes += costs[i] / 8.;
                if (x >= f.width || y >= f.height) {
                    map.unmappedDetailBytes += costs[i] / 8.;
                    continue;
                }
                map.tiles.push_back({x, y, std::min(f.width - x, uint32_t(extent) * scale),
                                     std::min(f.height - y, uint32_t(extent) * scaleY), uint32_t(c), uint32_t(level),
                                     costs[i], priorityRates[job].empty() ? 1.f : priorityRates[job][i],
                                     budgetPrecisions[job].empty() ? 0.f : budgetPrecisions[job][i]});
                const auto x0 = x / 16, y0 = y / 16, x1 = (std::min(f.width, x + extent * scale) + 15) / 16,
                           y1 = (std::min(f.height, y + extent * scaleY) + 15) / 16;
                if (level == 0 && !budgetPrecisions[job].empty() && budgetPrecisions[job][i] > 0)
                    for(uint32_t yy=y0;yy<y1;++yy) for(uint32_t xx=x0;xx<x1;++xx) {
                        map.localQuantSteps[c][size_t(yy)*map.cols+xx] += budgetPrecisions[job][i];
                        precisionCounts[c][size_t(yy)*map.cols+xx] += 1;
                    }
                const auto& coefficients=budgetCoefficients[job];
                const int tx=int(i%cols)*extent,ty=int(i/cols)*extent,
                          tw=std::min(extent,bw-tx),th=std::min(extent,std::min(rows,bh-offset)-ty);
                const float precision=budgetPrecisions[job].empty() || budgetPrecisions[job][i]==0
                                          ? 1.f : budgetPrecisions[job][i];
                auto weight=[&](int xx,int yy) {
                    const float q=std::abs(coefficients[size_t(ty+yy)*bw+tx+xx])/precision;
                    return q>0 ? 2. + 2.*std::log2(1.+q) : 0.;
                };
                double total=0;
                if(!coefficients.empty())for(int yy=0;yy<th;++yy)for(int xx=0;xx<tw;++xx)total+=weight(xx,yy);
                if(total>0) {
                    for(int yy=0;yy<th;++yy)for(int xx=0;xx<tw;++xx) {
                        const double share=weight(xx,yy);
                        if(share)mapFootprint(costs[i]*share/total,x+xx*scale,y+yy*scaleY,scale,scaleY,c);
                    }
                } else mapFootprint(costs[i],x,y,extent*scale,extent*scaleY,c);
            }
        }
        for(int c=0;c<3;++c) for(size_t i=0;i<cells;++i)
            if(precisionCounts[c][i]) map.localQuantSteps[c][i] /= precisionCounts[c][i];
        map.bits.resize(size_t(map.cols) * map.rows);
        for (uint32_t y = 0; y < map.rows; ++y)
            for (uint32_t x = 0; x < map.cols; ++x) {
                const auto at = size_t(y) * stride + x;
                double value = difference[at];
                if (x)
                    value += difference[at - 1];
                if (y)
                    value += difference[at - stride];
                if (x && y)
                    value -= difference[at - stride - 1];
                difference[at] = value;
                map.bits[size_t(y) * map.cols + x] = float(std::max(0., value));
            }
    }
    timing.mark("encode.rate_search_including_pack");
    require(result.data.size() <= maxPacket, "Encoded frame exceeds size limit.");
    if (sourceOutput) {
        sourceOutput->transformed = std::move(planes);
        sourceOutput->alpha = std::move(alpha);
        sourceOutput->opaque = opaque;
        sourceOutput->ready = true;
    }
    return result;
}

Packet encodeWavelet(const Frame& f, const EncodeOptions& o, bool allowFlatRegions, std::span<const uint8_t> cflMap,
                     uint32_t cflTile, std::span<const float> cflMeans, WaveletSourceColor* sourceColor) {
    return encodeWaveletImpl(f, o, allowFlatRegions, cflMap, cflTile, cflMeans, nullptr, -2, sourceColor);
}

template <bool debug>
static Frame decodeWaveletImpl(PacketView packet, uint32_t width, uint32_t height, uint32_t threads,
                               std::span<uint8_t> destination, uint32_t smoothing, uint32_t sharpening,
                               std::span<const uint8_t> predictor, std::vector<float>* lumaOutput, bool lumaOnly,
                               std::span<const uint8_t> cflMap, uint32_t cflTile, bool cflCentered, uint8_t chromaModel,
                               WaveletDebugCapture* capture, uint32_t nativeFilterStrength = 100) {
    require(chromaModel <= 3, "Invalid chroma reconstruction model.");
    TimingTimeline timing;
    require(smoothing <= 100 && sharpening <= 100 && nativeFilterStrength <= 100, "Invalid restoration strength.");
    require(destination.empty() || destination.size() == pixelBytes(width, height), "Invalid decode destination.");
    // Native reconstruction is a property of the packet. R12 v72 also needs
    // the native chroma interpolation, rather than nearest-neighbor colors.
    require(predictor.empty() || predictor.size() == pixelBytes(width, height), "Invalid screen predictor.");
    ByteReader input{packet.data};
    require(input.u32() == frameMagic && input.u8() == 114 && input.u8() == 3, "Unsupported native wavelet format.");
    constexpr int storedVersion = 114, version = 40;
    constexpr bool compactNative = true;
    const int q = input.u8(), flags = input.u8();
    require(q >= 1 && q <= 100 && flags <= 127 && input.u32() == width && input.u32() == height,
            "Invalid wavelet options.");
    const auto fixedStep = input.u16();
    require(fixedStep > 0, "Invalid quantizer.");
    const float baseStep = fixedStep / 16.f;
    const auto a = input.u8(), b = input.u8(), c = input.u8();
    require(a >= 100 && a <= 240 && b >= 64 && b <= 224 && c >= 32 && c <= 240, "Invalid coefficient allocation.");
    const float slope = a / 256.f, diagonal = b / 128.f, chromaScale = c / 128.f;
    const uint8_t restoration = input.u8();
    require(!restoration, "Unsupported retired restoration map.");
    const int alternativeKernel = input.u8();
    require(alternativeKernel == 10, "Unsupported retired transform.");
    uint8_t nativeStrength = input.u8();
    uint8_t nativeGuide = input.u8();
    const uint8_t nativeFilterCode = input.u8();
    const uint8_t nativeFilterPlanes = nativeFilterCode & 3;
    uint8_t nativeLumaCleanup = nativeFilterCode >> 2;
    const uint8_t nativeChromaCleanup = nativeGuide >> 6;
    nativeGuide &= 63;
    require(nativeStrength <= 64 && nativeGuide <= 32 && nativeLumaCleanup <= 14 && nativeFilterPlanes >= 1 &&
                nativeFilterPlanes <= 3,
            "Invalid reconstruction strengths.");
    const int predictionLevel = input.u8();
    require(predictionLevel <= 4, "Invalid prediction level.");
    const auto medianCode = input.u8();
    require(medianCode == 0, "Unsupported retired median mode.");
    const bool medianPrediction = false;
    constexpr uint8_t rgbRestoration = 0;
    const auto adaptiveQuantizer = input.u8();
    require(adaptiveQuantizer == 0 || adaptiveQuantizer == 3, "Invalid adaptive quantizer.");
    const uint8_t entropyCode = input.u8(), entropyMode = entropyCode & 63;
    const unsigned partitionCode = entropyCode & 192;
    require((entropyMode == 23 || entropyMode == 24 || entropyMode == 31 || entropyMode == 32) && partitionCode == 0,
            "Unsupported retired coefficient mode.");
    constexpr int nativeParts = 1;
    const uint8_t colorSpace = input.u8();
    require(colorSpace == 2, "Unsupported retired color space.");
    uint8_t lumaStrength = input.u8();
    require(lumaStrength <= 64, "Invalid luma strength.");
    // Validate stored controls before scaling. 100 preserves existing packets
    // exactly; zero disables native cleanup independently of saved R441 filters.
    nativeStrength = uint8_t((nativeStrength * nativeFilterStrength + 50) / 100);
    lumaStrength = uint8_t((lumaStrength * nativeFilterStrength + 50) / 100);
    nativeGuide = uint8_t((nativeGuide * nativeFilterStrength + 50) / 100);
    if (!nativeFilterStrength)
        nativeLumaCleanup = 0;
    Bytes restorationStorage;
    std::span<const uint8_t> restorationMap;
    constexpr int restorationTile = 64, restorationCols = 0;
    const bool chroma420 = (flags & 2) != 0, chroma422 = (flags & 64) != 0, opaque = (flags & 1) != 0,
               detail = (flags & 4) != 0;
    auto quantStep = [&](float base, int level, int band, bool) {
        if ((version != 27 && version != 29 && version != 40) || !slope)
            return stepFor(base, level, band, detail);
        return band < 0 ? std::max(.25f, base * .10f)
                        : std::max(.35f, base * (band == 2 ? diagonal : 1.f) * std::pow(slope, float(level)));
    };
    require(!(chroma420 && chroma422), "Conflicting stored chroma modes.");
    require(nativeLumaCleanup<8||chroma420,"Coarse color cleanup requires subsampled CDF 9/7.");
    require(!(flags & 8) || chroma420, "Invalid chroma allocation.");
    const int w = (int(width) + 63) & ~63, h = (int(height) + 63) & ~63;
    const int cw = (chroma420 || chroma422) ? ((int(width) + 1) / 2 + 63) & ~63 : w,
              ch = chroma420 ? ((int(height) + 1) / 2 + 63) & ~63 : h;
    std::array<std::span<const uint8_t>, 17> coded;
    for (auto& stream : coded)
        stream = input.bytes(compactNative ? input.var() : input.u32());
    input.end();

    std::array<Bytes, 17> storage;
    parallelFor(17, threads, [&](uint32_t i) {
        if (lumaOnly && (i < 1 || i > 5))
            return;
        size_t cap = i == 0 && (version == 18 || version == 20 || (version == 27 || (version == 29 || version == 40)))
                         ? 34 + size_t((width + 7) / 8) * ((height + 7) / 8)
                         : 0;
        if (i == 16)
            cap = opaque ? 0 : size_t(width) * height;
        else if (i) {
            const int channel = (i - 1) / 5, pw = channel ? cw : w, ph = channel ? ch : h;
            const size_t area = size_t(pw) * ph;
            const int kind = (i - 1) % 5;
            cap = kind == 0   ? size_t(pw >> levels) * (ph >> levels) * 5
                  : kind == 1 ? area + 18
                              : ((area - area / 4096) / 3) * 5;
        }
        if (compactNative && coded[i].empty())
            storage[i].clear();
        else
            storage[i] = entropyDecode(coded[i], cap);
    });
    int flatTile = 16, flatCols = 0;
    std::array<uint32_t, 9> flatPalette{};
    std::span<const uint8_t> flatMap;
    if (version == 18 ||
        ((version == 20 || (version == 27 || (version == 29 || version == 40))) && !storage[0].empty())) {
        auto layout = readFlatRegions(storage[0], width, height);
        flatTile = int(layout.tile);
        flatCols = int(layout.cols);
        flatPalette = layout.palette;
        flatMap = layout.map;
    } else
        require(storage[0].empty(), "Invalid wavelet control stream.");
    thread_local std::array<std::vector<float>, 3> cachedPlanes;
    std::array<std::vector<float>, 3> largePlanes;
    auto& planes = !debug && size_t(w) * h <= cachedSamples ? cachedPlanes : largePlanes;
    struct BandView {
        std::span<const uint8_t> runs, values;
    };
    std::array<BandView, 216> views;
    timing.mark("decode.header_entropy");
    parallelFor(lumaOnly ? 1 : 3, threads, [&](uint32_t c) {
        const int pw = c ? cw : w, ph = c ? ch : h, base = 1 + c * 5;
        auto& data = planes[c];
        data.resize(size_t(pw) * ph);
        std::array<ByteReader, 5> streams;
        for (int i = 0; i < 5; ++i)
            streams[i] = {storage[base + i]};
        const float color = c ? ((version == 27 || (version == 29 || version == 40)) ? chromaScale
                                 : (version >= 15 && (flags & 8))                    ? .8f
                                                                                     : 1.2f)
                              : 1.f,
                    dcStep = quantStep(baseStep, 0, -1, detail) * color;
        const int lw = pw >> levels, lh = ph >> levels;
        std::vector<int> previous(lw);
        for (int y = 0; y < lh; ++y) {
            int left = 0, corner = 0;
            for (int x = 0; x < lw; ++x) {
                const int top = previous[x];
                const int64_t value = int64_t(medianPredict(left, top, corner)) + streams[0].signedVar();
                require(std::abs(value) <= 1048576, "Invalid wavelet DC.");
                previous[x] = int(value);
                corner = top;
                left = int(value);
                data[size_t(y) * pw + x] = float(value) * dcStep;
            }
        }
        bands(pw, ph, [&](int level, int band, int, int, int bw, int bh) {
            size_t pos = 0;
            const size_t area = size_t(bw) * bh, runStart = streams[1].pos, valueStart = streams[2 + band].pos;
            if (version >= 16) {
                require(streams[1].data.empty(), "Invalid spatial runs.");
                auto& values = streams[2 + band];
                const int rows =
                    (version == 29 || version == 40) ? spatialRows(bw, bh, nativeParts) : spatialRows(bw, bh);
                for (int part = 0; part * rows < bh; ++part)
                    views[(c * 18 + level * 3 + band) * 4 + part] = {
                        {}, values.bytes((version == 29 || version == 40) ? values.var() : values.u32())};
                return;
            }
            while (true) {
                const auto skip = streams[1].var();
                if (!skip)
                    break;
                require(skip <= area - pos, "Invalid wavelet run.");
                pos += skip;
                streams[2 + band].var();
            }
            views[c * 18 + level * 3 + band] = {
                streams[1].data.subspan(runStart, streams[1].pos - runStart),
                streams[2 + band].data.subspan(valueStart, streams[2 + band].pos - valueStart)};
        });
        for (auto& stream : streams)
            stream.end();
    });
    timing.mark("decode.layout_dc");
    const bool wholeBands = (version == 29 || version == 40) && nativeParts == 1;
    auto decodeBand = [&](uint32_t job) {
        const uint32_t viewIndex = wholeBands ? job * 4 : job;
        const int index = wholeBands      ? job
                          : version >= 16 ? job / 4
                                          : job,
                  c = index / 18, level = (index % 18) / 3, band = index % 3, pw = c ? cw : w, ph = c ? ch : h,
                  bw = (pw >> level) / 2, bh = (ph >> level) / 2, bx = band == 1 ? 0 : bw, by = band == 0 ? 0 : bh;
        ByteReader runs{views[viewIndex].runs}, values{views[viewIndex].values};
        auto& data = planes[c];
        const float step = quantStep(baseStep, level, band, detail) *
                           (c ? ((version == 27 || (version == 29 || version == 40)) ? chromaScale
                                 : (version >= 15 && (flags & 8))                    ? .8f
                                                                                     : 1.2f)
                              : 1.f);
        size_t pos = 0;
        const int rows = (version == 29 || version == 40) ? spatialRows(bw, bh, nativeParts)
                         : version >= 16                  ? spatialRows(bw, bh)
                                                          : bh,
                  offset = !wholeBands && version >= 16 ? int(job % 4) * rows : 0;
        if (offset >= bh)
            return;
        for (int y = offset; y < std::min(bh, offset + rows); ++y)
            std::fill_n(data.data() + size_t(by + y) * pw + bx, bw, 0.f);
        if ((version == 29 || version == 40) && views[viewIndex].values.empty())
            return;
        if (version >= 16) {
            decodeSpatialBand(
                views[viewIndex].values, data.data() + size_t(by + offset) * pw + bx, pw, bw,
                std::min(rows, bh - offset), step, detail && !((version == 29 || version == 40) && (flags & 32)),
                (version == 20 || ((version == 27 || (version == 29 || version == 40)) && (flags & 16))) &&
                    level >= predictionLevel,
                medianPrediction, adaptiveQuantizer != 0,
                (storedVersion == 78 ||
                 (storedVersion == 81 || (storedVersion == 82 || storedVersion == 89 || storedVersion == 90 ||
                                          (storedVersion == 91 || storedVersion == 114)))),
                (storedVersion == 82 || storedVersion == 89 || storedVersion == 90 ||
                 (storedVersion == 91 || storedVersion == 114)),
                storedVersion == 89 && level + 1 < levels
                    ? data.data() + size_t(band == 0 ? 0 : bh / 2) * pw + (band == 1 ? 0 : bw / 2)
                    : nullptr,
                pw, entropyMode);
            return;
        }
        while (true) {
            const auto skip = runs.var();
            if (!skip)
                break;
            pos += skip - 1;
            const int value = values.signedVar();
            require(value != 0 && std::abs(int64_t(value)) <= 1048576, "Invalid wavelet coefficient.");
            size_t x = pos % bw, y = pos / bw;
            if (detail) {
                const uint32_t stripe = uint32_t(pos) / (uint32_t(bw) * 16),
                               stripeHeight = std::min(16u, uint32_t(bh) - stripe * 16),
                               local = uint32_t(pos) - stripe * uint32_t(bw) * 16,
                               column = stripeHeight == 16 ? local >> 8 : local / (stripeHeight * 16),
                               tileWidth = std::min(16u, uint32_t(bw) - column * 16),
                               inside = local - column * stripeHeight * 16;
                x = column * 16 + (tileWidth == 16 ? inside & 15 : inside % tileWidth);
                y = stripe * 16 + (tileWidth == 16 ? inside >> 4 : inside / tileWidth);
            }
            const float restored = detail ? float(value) + std::copysign(.15f, float(value)) : float(value);
            data[size_t(by + y) * pw + bx + x] = restored * step;
            ++pos;
        }
        runs.end();
        values.end();
    };
    if (storedVersion == 89) {
        for (int level = levels - 1; level >= 0; --level)
            parallelFor(lumaOnly ? 3 : 9, threads,
                        [&](uint32_t job) { decodeBand((job / 3) * 18 + level * 3 + job % 3); });
    } else
        parallelFor(lumaOnly ? (wholeBands      ? 18
                                : version >= 16 ? 72
                                                : 18)
                             : (wholeBands      ? 54
                                : version >= 16 ? 216
                                                : 54),
                    threads, decodeBand);
    if constexpr (debug)
        for (size_t slot = 0; slot < 2; ++slot) {
            const int view = capture->views[slot];
            if (view >= 7 && view <= 9) {
                const int c = view - 7;
                capture->plane(slot, planes[c], c ? cw : w, c ? cw : w, c ? ch : h, true);
            }
        }
    timing.mark("decode.detail_coefficients");
    // Retain the historical row evaluation and all source-guided cleanup paths.
    std::array<GpuPlane,3> gpuPlanes{{{planes[0],uint32_t(w),uint32_t(h)},
        {planes[1],uint32_t(cw),uint32_t(ch)},{planes[2],uint32_t(cw),uint32_t(ch)}}};
    if constexpr(!debug) {
        if(computeBackend()==ComputeBackend::Gpu&&!nativeChromaCleanup&&!lumaOnly&&!lumaOutput&&opaque&&predictor.empty()&&flatMap.empty()&&!sharpening&&(cflMap.empty()||cflCentered)){
            require(storage[16].empty(),"Opaque wavelet has an alpha stream.");
            Frame result{width,height,packet.pts,packet.duration,destination.empty()?Bytes(pixelBytes(width,height)):Bytes{}};
            const auto output=destination.empty()?std::span(result.pixels):destination;
            GpuNativeSettings settings{width,height,smoothing,nativeGuide,cflTile,chroma420,chroma422,cflMap,nativeLumaCleanup,nativeStrength,lumaStrength,nativeFilterPlanes};
            if(gpuDecodeNative(gpuPlanes,settings,output)){timing.mark("decode.gpu_native");return result;}
        }
    }
    if (storedVersion == 114 && chroma420 && !nativeLumaCleanup && !lumaOnly && alternativeKernel == 10 && hasAvx2() &&
        workerLimit(threads) >= 3) {
        inverseCdfPlanes(planes, w, h, cw, ch, threads, version != 10);
    } else
        for (int c = 0; c < (lumaOnly ? 1 : 3); ++c) {
            if (c == 0 && nativeLumaCleanup) {
                require(alternativeKernel == 10, "Luma cleanup requires CDF 9/7.");
                waveletTransform(planes[0], w, h, threads, true, true, true, 1, 5);
                recursiveLumaCleanup(planes[0], w, w / 2, h / 2, threads,
                                     nativeLumaCleanup >= 8 ? 5 : nativeLumaCleanup);
                waveletTransform(planes[0], w, h, threads, true, true, true, 0, 1);
            } else if (c == 1 && nativeLumaCleanup >= 8) {
                require(alternativeKernel == 10 && chroma420, "Coarse color cleanup requires subsampled CDF 9/7.");
                const uint32_t workers = workerLimit(threads), perChannel = std::max(1u, workers / 2);
                auto inverseChroma = [&](int first, int count) {
                    // count already limits outer participants to two. Passing
                    // two as the total budget also clipped both child loops.
                    parallelFor(2, workers, [&](uint32_t channel) {
                        waveletTransform(planes[channel + 1], cw, ch, perChannel, true, true, true, first, count);
                    });
                };
                inverseChroma(1, 5);
                const std::vector<float> guide;
                recursiveChromaCleanup(
                    planes[1], planes[2], guide, cw, cw / 2, ch / 2, threads,
                    uint8_t(nativeLumaCleanup >= 12 ? nativeLumaCleanup - 11 : nativeLumaCleanup - 4));
                inverseChroma(0, 1);
            } else if (c == 2 && nativeLumaCleanup >= 8)
                continue;
            else
                transformNativePlane(planes[c], c ? cw : w, c ? ch : h, threads, true,
                                     nativeKernelForChannel(alternativeKernel, c), version != 10);
        }
    timing.mark("decode.inverse_wavelet");
    if (nativeStrength) {
        for (int c = 0; c < (lumaOnly ? 1 : 3); ++c)
            if (nativeFilterPlanes & (c ? 2 : 1))
                restoreNativePlane(planes[c], c ? cw : w,
                                   c && (chroma420 || chroma422) ? int((width + 1) / 2) : int(width),
                                   c && chroma420 ? int((height + 1) / 2) : int(height), threads,
                                   c ? nativeStrength * .5f : lumaStrength, uint32_t(c));
        timing.mark("decode.native_directional_restoration");
    }
    if constexpr (debug)
        for (size_t slot = 0; slot < 2; ++slot) {
            const int view = capture->views[slot];
            if (view == 2 || view == 5 || view == 6) {
                const int c = view == 2 ? 0 : view - 4;
                capture->plane(slot, planes[c], c ? cw : w, c && (chroma420 || chroma422) ? (width + 1) / 2 : width,
                               c && chroma420 ? (height + 1) / 2 : height, false);
            }
            if (view == 3 || view == 4)
                capture->images[slot] = {width, height, 0, 0, Bytes(pixelBytes(width, height))};
        }
    if (lumaOutput)
        *lumaOutput = planes[0];
    if (lumaOnly)
        return {width, height, packet.pts, packet.duration, {}};
    require(cflMap.empty() ||
                (cflTile >= 16 && cflTile <= 128 &&
                 cflMap.size() == size_t((width + cflTile - 1) / cflTile) * ((height + cflTile - 1) / cflTile) * 2),
            "Invalid native chroma predictor.");
    std::vector<float> cflAverages;
    if (cflCentered && !cflMap.empty()) {
        const uint32_t cols = (width + cflTile - 1) / cflTile, rows = (height + cflTile - 1) / cflTile;
        cflAverages.resize(size_t(cols) * rows);
        parallelFor(rows, threads, [&](uint32_t ty) {
            for (uint32_t tx = 0; tx < cols; ++tx) {
                double sum = 0;
                uint32_t count = 0;
                for (uint32_t y = ty * cflTile; y < std::min(height, (ty + 1) * cflTile); ++y)
                    for (uint32_t x = tx * cflTile; x < std::min(width, (tx + 1) * cflTile); ++x) {
                        sum += planes[0][size_t(y) * w + x];
                        ++count;
                    }
                cflAverages[size_t(ty) * cols + tx] = float(sum / count);
            }
        });
    }
    bool display420 = chroma420;
    int displayCw = cw, displayCh = ch;
    const float* displayPlanes[3] = {planes[0].data(), planes[1].data(), planes[2].data()};
    thread_local std::array<std::vector<float>, 3> guidedCache;
    std::array<std::vector<float>, 3> guidedOwned;
    auto& guided = !debug && size_t(w) * h <= cachedSamples ? guidedCache : guidedOwned;
    if (chroma422) {
        for (int c = 1; c < 3; ++c)
            guided[c].resize(size_t(w) * h);
        parallelFor(height, threads, [&](uint32_t y) {
            for (uint32_t x = 0; x < width; ++x) {
                const int center = int(x / 2),
                          neighbor = std::clamp(center + (x % 2 ? 1 : -1), 0, int((width + 1) / 2) - 1);
                for (int c = 1; c < 3; ++c) {
                    const float value = planes[c][size_t(y) * cw + center];
                    guided[c][size_t(y) * w + x] =
                        smoothing ? value * .75f + planes[c][size_t(y) * cw + neighbor] * .25f : value;
                }
            }
        });
        displayCw = w;
        displayCh = h;
        displayPlanes[1] = guided[1].data();
        displayPlanes[2] = guided[2].data();
    }
    const bool fusedNativeGuide = storedVersion == 114 && nativeGuide && restoration == 0 && chroma420 &&
                                  predictor.empty() && sharpening == 0 && hasAvx2();
    if ((version == 27 || (version == 29 || version == 40)) && (restoration || nativeGuide) &&
        (smoothing || nativeGuide) && chroma420 && predictor.empty()) {
        auto& guides = guided[0];
        guides.resize(size_t(cw) * ch);
        parallelFor(uint32_t((height + 1) / 2), threads, [&](uint32_t y) {
            for (uint32_t x = 0; x < (width + 1) / 2; ++x) {
                const auto ax = x * 2, ay = y * 2;
                guides[size_t(y) * cw + x] =
                    (planes[0][size_t(ay) * w + ax] + planes[0][size_t(ay) * w + std::min(width - 1, ax + 1)] +
                     planes[0][size_t(std::min(height - 1, ay + 1)) * w + ax] +
                     planes[0][size_t(std::min(height - 1, ay + 1)) * w + std::min(width - 1, ax + 1)]) *
                    .25f;
            }
        });
        if (nativeChromaCleanup) {
            recursiveChromaCleanup(planes[1], planes[2], guides, cw, int((width + 1) / 2), int((height + 1) / 2),
                                   threads, nativeChromaCleanup);
            timing.mark("decode.recursive_chroma_cleanup");
        }
        if (!fusedNativeGuide) {
            std::span<std::vector<float>, 2> restored(guided.data() + 1, 2);
            for (auto& v : restored)
                v.resize(size_t(w) * h);
            const float guideThreshold = nativeGuide        ? float(nativeGuide)
                                         : restoration == 2 ? 16.f
                                         : restoration == 3 ? 12.f
                                                            : std::clamp(baseStep * .08f, 6.f, 18.f),
                        inverseGuide = 1 / (guideThreshold * guideThreshold);
            parallelFor(height, threads, [&](uint32_t y) {
                const int cy = y / 2, ny = std::clamp(cy + (y % 2 ? 1 : -1), 0, int((height + 1) / 2) - 1),
                          last = int((width + 1) / 2) - 1;
                const auto one = _mm_set1_ps(1.f), inv = _mm_set1_ps(inverseGuide);
                uint32_t x = 0;
                const float n = nativeGuide ? .25f : .25f * smoothing / 100.f;
                const float basis[4] = {(1 - n) * (1 - n), (1 - n) * n, (1 - n) * n, n * n};
                auto gather = [&](const float* p, bool neighbor, int cx) {
                    return neighbor ? _mm_setr_ps(p[std::max(0, cx - 1)], p[cx + 1], p[cx], p[std::min(last, cx + 2)])
                                    : _mm_setr_ps(p[cx], p[cx], p[cx + 1], p[cx + 1]);
                };
                for (; x + 4 <= width; x += 4) {
                    const int cx = x / 2;
                    const auto luma = _mm_loadu_ps(planes[0].data() + size_t(y) * w + x);
                    __m128 g[4] = {gather(guides.data() + size_t(cy) * cw, false, cx),
                                   gather(guides.data() + size_t(cy) * cw, true, cx),
                                   gather(guides.data() + size_t(ny) * cw, false, cx),
                                   gather(guides.data() + size_t(ny) * cw, true, cx)};
                    const float* base = basis;
                    __m128 weights[4], total = _mm_setzero_ps();
                    __m128 range = _mm_sub_ps(_mm_max_ps(_mm_max_ps(g[0], g[1]), _mm_max_ps(g[2], g[3])),
                                              _mm_min_ps(_mm_min_ps(g[0], g[1]), _mm_min_ps(g[2], g[3])));
                    const auto gate =
                        restoration == 4
                            ? _mm_set1_ps(
                                  restorationMap[size_t(y / restorationTile) * restorationCols + x / restorationTile]
                                      ? 1.f
                                      : 0.f)
                            : _mm_cmpgt_ps(range, _mm_set1_ps(24.f));
                    for (int i = 0; i < 4; ++i) {
                        const auto d = _mm_sub_ps(luma, g[i]);
                        weights[i] =
                            _mm_div_ps(_mm_set1_ps(base[i]), _mm_add_ps(one, _mm_mul_ps(_mm_mul_ps(d, d), inv)));
                        if (restoration == 3 || restoration == 4) {
                            const auto mask = restoration == 4 ? _mm_cmpgt_ps(gate, _mm_setzero_ps()) : gate;
                            weights[i] =
                                _mm_or_ps(_mm_and_ps(mask, weights[i]), _mm_andnot_ps(mask, _mm_set1_ps(base[i])));
                        }
                        total = _mm_add_ps(total, weights[i]);
                    }
                    for (int c = 0; c < 2; ++c) {
                        __m128 v = _mm_setzero_ps();
                        for (int i = 0; i < 4; ++i)
                            v = _mm_add_ps(
                                v, _mm_mul_ps(weights[i], gather(planes[c + 1].data() + size_t(i >= 2 ? ny : cy) * cw,
                                                                 i % 2 != 0, cx)));
                        _mm_storeu_ps(restored[c].data() + size_t(y) * w + x, _mm_div_ps(v, total));
                    }
                }
                for (; x < width; ++x) {
                    const int cx = x / 2, nx = std::clamp(cx + (x % 2 ? 1 : -1), 0, last);
                    const size_t at[4] = {size_t(cy) * cw + cx, size_t(cy) * cw + nx, size_t(ny) * cw + cx,
                                          size_t(ny) * cw + nx};
                    const float* base = basis;
                    float weights[4], total = 0, lo = 1e10f, hi = -1e10f;
                    for (auto i : at) {
                        lo = std::min(lo, guides[i]);
                        hi = std::max(hi, guides[i]);
                    }
                    const float luma = planes[0][size_t(y) * w + x];
                    for (int i = 0; i < 4; ++i) {
                        const float d = luma - guides[at[i]];
                        weights[i] =
                            ((restoration == 3 && hi - lo <= 24) ||
                             (restoration == 4 &&
                              !restorationMap[size_t(y / restorationTile) * restorationCols + x / restorationTile]))
                                ? base[i]
                                : base[i] / (1 + d * d * inverseGuide);
                        total += weights[i];
                    }
                    for (int c = 0; c < 2; ++c) {
                        float v = 0;
                        for (int i = 0; i < 4; ++i)
                            v += weights[i] * planes[c + 1][at[i]];
                        restored[c][size_t(y) * w + x] = v / total;
                    }
                }
            });
            displayPlanes[1] = restored[0].data();
            displayPlanes[2] = restored[1].data();
            display420 = false;
            displayCw = w;
            displayCh = h;
        }
    }
    timing.mark("decode.guided_chroma");
    Frame frame{width, height, packet.pts, packet.duration,
                destination.empty() ? Bytes(pixelBytes(width, height)) : Bytes{}};
    const auto pixels = destination.empty() ? std::span(frame.pixels) : destination;
    const float neighborWeight = .25f * smoothing / 100.f, centerWeight = 1 - neighborWeight;
    auto chroma4 = [&](int c, uint32_t x, uint32_t y) {
        const auto* p = displayPlanes[c];
        if (!display420)
            return _mm_loadu_ps(p + size_t(y) * displayCw + x);
        const int cx = int(x / 2), cy = int(y / 2);
        if (!smoothing) {
            const auto pair =
                _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p + size_t(cy) * displayCw + cx)));
            return _mm_unpacklo_ps(pair, pair);
        }
        const int ny = std::clamp(y % 2 ? cy + 1 : cy - 1, 0, displayCh - 1);
        auto load = [&](int r) {
            const float* a = p + size_t(r) * displayCw;
            if (cx == 0)
                return _mm_loadu_ps(a);
            if (cx + 2 < displayCw)
                return _mm_loadu_ps(a + cx - 1);
            return _mm_set_ps(a[displayCw - 1], a[cx + 1], a[cx], a[cx - 1]);
        };
        const auto values = _mm_add_ps(_mm_mul_ps(load(cy), _mm_set1_ps(centerWeight)),
                                       _mm_mul_ps(load(ny), _mm_set1_ps(neighborWeight)));
        const auto center = cx ? _mm_shuffle_ps(values, values, _MM_SHUFFLE(2, 2, 1, 1))
                               : _mm_shuffle_ps(values, values, _MM_SHUFFLE(1, 1, 0, 0));
        const auto neighbor = cx ? _mm_shuffle_ps(values, values, _MM_SHUFFLE(3, 1, 2, 0))
                                 : _mm_shuffle_ps(values, values, _MM_SHUFFLE(2, 0, 1, 0));
        return _mm_add_ps(_mm_mul_ps(center, _mm_set1_ps(centerWeight)),
                          _mm_mul_ps(neighbor, _mm_set1_ps(neighborWeight)));
    };
    auto chroma1 = [&](int c, uint32_t x, uint32_t y) {
        const auto* p = displayPlanes[c];
        if (!display420)
            return p[size_t(y) * displayCw + x];
        const int cx = int(x / 2), cy = int(y / 2), nx = std::clamp(cx + (x % 2 ? 1 : -1), 0, displayCw - 1),
                  ny = std::clamp(cy + (y % 2 ? 1 : -1), 0, displayCh - 1);
        if (!smoothing)
            return p[size_t(cy) * displayCw + cx];
        return centerWeight * centerWeight * p[size_t(cy) * displayCw + cx] +
               centerWeight * neighborWeight * p[size_t(cy) * displayCw + nx] +
               centerWeight * neighborWeight * p[size_t(ny) * displayCw + cx] +
               neighborWeight * neighborWeight * p[size_t(ny) * displayCw + nx];
    };
    const auto zero = _mm_setzero_ps(), limit = _mm_set1_ps(255.f), half = _mm_set1_ps(.5f);
    auto toInt = [&](__m128 v) { return _mm_cvttps_epi32(_mm_add_ps(half, _mm_min_ps(limit, _mm_max_ps(zero, v)))); };
    const bool colorAvx2 = hasAvx2();
    const float sharp =
        version != 29 && version >= 14
            ? sharpening * .01f *
                  std::min((version == 20 || ((version == 27 || (version == 29 || version == 40)) && (flags & 2)))
                               ? .25f
                               : 1.f,
                           baseStep / 60.f)
            : 0.f;
    // Compact bilateral restoration: attenuate small ripples, preserve strong
    // glyph boundaries. Fused into the color conversion, without a frame pass.
    const float deringing = (version == 20 || version == 27) && detail && predictor.empty()
                                ? std::clamp(baseStep * .4f, 6.f, 24.f) * smoothing * .01f
                                : 0.f;
    parallelFor((height + 15) / 16, threads, [&](uint32_t job) {
        for (uint32_t y = job * 16; y < std::min(height, job * 16 + 16); ++y) {
            const auto* luma = planes[0].data() + size_t(y) * w;
            const auto* upper = planes[0].data() + size_t(y ? y - 1 : 0) * w;
            const auto* lower = planes[0].data() + size_t(std::min(height - 1, y + 1)) * w;
            thread_local std::array<std::vector<float>, 2> colorCorrection;
            const bool cfl = !cflMap.empty();
            if (cfl && cflCentered) {
                for (auto& v : colorCorrection)
                    v.resize(width);
                r6CorrectionRow(cflMap, cflAverages, cflTile, int((width + cflTile - 1) / cflTile),
                                int((height + cflTile - 1) / cflTile), float(y), width, luma, colorCorrection[0].data(),
                                colorCorrection[1].data(),
                                (storedVersion == 82 || storedVersion == 89 || storedVersion == 90 ||
                                 (storedVersion == 91 || storedVersion == 114)));
            } else if (cfl) {
                const uint32_t cols = (width + cflTile - 1) / cflTile, ay = y & ~1u, by = std::min(height - 1, ay + 1);
                const auto* a = planes[0].data() + size_t(ay) * w;
                const auto* b = planes[0].data() + size_t(by) * w;
                for (auto& v : colorCorrection)
                    v.resize(width);
                for (uint32_t xx = 0; xx < width; xx += 2) {
                    const uint32_t nx = std::min(width - 1, xx + 1);
                    const float average = ((a[xx] + a[nx]) + (b[xx] + b[nx])) * .25f;
                    const size_t cell = (size_t(y / cflTile) * cols + xx / cflTile) * 2;
                    for (int c = 0; c < 2; ++c) {
                        const float alpha = int8_t(cflMap[cell + c]) / 16.f;
                        colorCorrection[c][xx] = alpha * (luma[xx] - average);
                        if (nx != xx)
                            colorCorrection[c][nx] = alpha * (luma[nx] - average);
                    }
                }
            }
            const float* rowColor[2] = {displayPlanes[1], displayPlanes[2]};
            int rowCw = displayCw, rowCh = displayCh;
            uint32_t colorY = y;
            bool row420 = display420;
            if (fusedNativeGuide) {
                thread_local std::array<std::vector<float>, 2> rows;
                for (auto& row : rows)
                    row.resize(width);
                const int cy = int(y / 2), ny = std::clamp(cy + (y % 2 ? 1 : -1), 0, int((height + 1) / 2) - 1);
                nativeGuidedChromaRow(luma, guided[0].data() + size_t(cy) * cw, guided[0].data() + size_t(ny) * cw,
                                      planes[1].data() + size_t(cy) * cw, planes[1].data() + size_t(ny) * cw,
                                      planes[2].data() + size_t(cy) * cw, planes[2].data() + size_t(ny) * cw,
                                      int((width + 1) / 2) - 1, width, 1.f / (float(nativeGuide) * nativeGuide),
                                      rows[0].data(), rows[1].data(), true, chromaModel);
                rowColor[0] = rows[0].data();
                rowColor[1] = rows[1].data();
                rowCw = int(width);
                rowCh = 1;
                colorY = 0;
                row420 = false;
            }
            const auto* base = predictor.empty() ? nullptr : predictor.data() + size_t(y) * width * 4;
            uint32_t x = colorAvx2 && !colorSpace
                             ? waveletColorRowAvx2(
                                   luma, rowColor[0], rowColor[1], rowCw, rowCh, width, colorY, row420, smoothing,
                                   pixels.data() + size_t(y) * width * 4, upper, lower, sharp, base, deringing,
                                   flatMap.empty() ? nullptr : flatMap.data() + size_t(y / flatTile) * flatCols,
                                   flatPalette.data(), uint32_t(flatTile), cfl ? colorCorrection[0].data() : nullptr,
                                   cfl ? colorCorrection[1].data() : nullptr)
                             : 0;
            if (colorAvx2 && colorSpace == 2 && !sharp && !deringing && !base && flatMap.empty())
                x = waveletOklabColorRowAvx2(
                    luma, rowColor[0], rowColor[1], rowCw, rowCh, width, colorY, row420, smoothing,
                    pixels.data() + size_t(y) * width * 4, cfl ? colorCorrection[0].data() : nullptr,
                    cfl ? colorCorrection[1].data() : nullptr, perceptualColor::gammaTable().data());
            if (colorAvx2 && colorSpace >= 3 && !sharp && !deringing && !base && flatMap.empty())
                x = waveletProxyColorRowAvx2(luma, rowColor[0], rowColor[1], rowCw, rowCh, width, colorY, row420,
                                             smoothing, pixels.data() + size_t(y) * width * 4,
                                             cfl ? colorCorrection[0].data() : nullptr,
                                             cfl ? colorCorrection[1].data() : nullptr, colorSpace);
            if constexpr (debug)
                for (size_t slot = 0; slot < 2; ++slot)
                    if (capture->views[slot] == 3 || capture->views[slot] == 4) {
                        const int c = capture->views[slot] - 3;
                        for (uint32_t at = 0; at < x; ++at) {
                            const float value = (fusedNativeGuide ? rowColor[c][at] : chroma1(c + 1, at, y)) +
                                                (cfl ? colorCorrection[c][at] : 0.f);
                            capture->pixel(slot, at, y, value);
                        }
                    }
            const uint32_t vectorEnd = x;
            auto light = [&](uint32_t x) {
                float center = luma[x];
                if (!sharp && !deringing)
                    return center;
                const float up = upper[x], down = lower[x], left = luma[x ? x - 1 : 0],
                            right = luma[std::min(width - 1, x + 1)], average = ((up + down) + (left + right)) * .25f;
                const float minimum = std::min(center, std::min(std::min(up, down), std::min(left, right))),
                            maximum = std::max(center, std::max(std::max(up, down), std::max(left, right)));
                if (deringing) {
                    auto contribution = [&](float v) {
                        const float d = v - center;
                        return std::abs(d) < deringing ? d : 0.f;
                    };
                    center +=
                        ((contribution(up) + contribution(down)) + (contribution(left) + contribution(right))) * .125f;
                }
                return std::clamp(center + (center - average) * sharp, minimum, maximum);
            };
            for (; x + 4 <= width; x += 4) {
                const auto yy =
                    _mm_add_ps(sharp || deringing ? _mm_set_ps(light(x + 3), light(x + 2), light(x + 1), light(x))
                                                  : _mm_loadu_ps(luma + x),
                               _mm_set1_ps(128));
                auto cb = fusedNativeGuide ? _mm_loadu_ps(rowColor[0] + x) : chroma4(1, x, y),
                     cr = fusedNativeGuide ? _mm_loadu_ps(rowColor[1] + x) : chroma4(2, x, y);
                if (cfl) {
                    cb = _mm_add_ps(cb, _mm_loadu_ps(colorCorrection[0].data() + x));
                    cr = _mm_add_ps(cr, _mm_loadu_ps(colorCorrection[1].data() + x));
                }
                if constexpr (debug) {
                    alignas(16) float values[2][4];
                    _mm_store_ps(values[0], cb);
                    _mm_store_ps(values[1], cr);
                    for (size_t slot = 0; slot < 2; ++slot)
                        if (capture->views[slot] == 3 || capture->views[slot] == 4)
                            for (uint32_t lane = 0; lane < 4; ++lane)
                                capture->pixel(slot, x + lane, y, values[capture->views[slot] - 3][lane]);
                }
                __m128 rr, gg, bb;
                if (colorSpace >= 3) {
                    __m128 v[3];
                    const int axis = colorSpace - 3;
                    v[axis] = yy;
                    v[(axis + 1) % 3] = _mm_add_ps(yy, _mm_add_ps(cb, cb));
                    v[(axis + 2) % 3] = _mm_add_ps(yy, _mm_add_ps(cr, cr));
                    rr = v[0];
                    gg = v[1];
                    bb = v[2];
                } else if (colorSpace == 2)
                    perceptualColor::inverse4(yy, cb, cr, rr, gg, bb);
                else {
                    rr = colorSpace ? _mm_sub_ps(_mm_add_ps(yy, cb), cr)
                                    : _mm_add_ps(yy, _mm_mul_ps(cr, _mm_set1_ps(1.402f)));
                    gg = colorSpace ? _mm_add_ps(yy, cr)
                                    : _mm_sub_ps(_mm_sub_ps(yy, _mm_mul_ps(cb, _mm_set1_ps(.344136f))),
                                                 _mm_mul_ps(cr, _mm_set1_ps(.714136f)));
                    bb = colorSpace ? _mm_sub_ps(_mm_sub_ps(yy, cb), cr)
                                    : _mm_add_ps(yy, _mm_mul_ps(cb, _mm_set1_ps(1.772f)));
                }
                const auto r = toInt(rr), g = toInt(gg), b = toInt(bb);
                auto rgba = _mm_or_si128(_mm_or_si128(r, _mm_slli_epi32(g, 8)),
                                         _mm_or_si128(_mm_slli_epi32(b, 16), _mm_set1_epi32(int(0xff000000u))));
                if (base) {
                    auto original = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + x * 4));
                    const auto z = _mm_setzero_si128(), offset = _mm_set1_epi16(256);
                    auto lo = _mm_sub_epi16(_mm_add_epi16(_mm_unpacklo_epi8(original, z),
                                                          _mm_slli_epi16(_mm_unpacklo_epi8(rgba, z), 1)),
                                            offset),
                         hi = _mm_sub_epi16(_mm_add_epi16(_mm_unpackhi_epi8(original, z),
                                                          _mm_slli_epi16(_mm_unpackhi_epi8(rgba, z), 1)),
                                            offset);
                    rgba = _mm_or_si128(_mm_packus_epi16(lo, hi), _mm_set1_epi32(int(0xff000000u)));
                }
                _mm_storeu_si128(reinterpret_cast<__m128i*>(pixels.data() + (size_t(y) * width + x) * 4), rgba);
            }
            for (; x < width; ++x) {
                const float yy = light(x) + 128;
                float cb = fusedNativeGuide ? rowColor[0][x] : chroma1(1, x, y),
                      cr = fusedNativeGuide ? rowColor[1][x] : chroma1(2, x, y);
                if (cfl) {
                    cb += colorCorrection[0][x];
                    cr += colorCorrection[1][x];
                }
                if constexpr (debug)
                    for (size_t slot = 0; slot < 2; ++slot)
                        if (capture->views[slot] == 3 || capture->views[slot] == 4)
                            capture->pixel(slot, x, y, capture->views[slot] == 3 ? cb : cr);
                auto* p = pixels.data() + (size_t(y) * width + x) * 4;
                if (colorSpace >= 3) {
                    float v[3];
                    const int axis = colorSpace - 3;
                    v[axis] = yy;
                    v[(axis + 1) % 3] = yy + 2 * cb;
                    v[(axis + 2) % 3] = yy + 2 * cr;
                    for (int c = 0; c < 3; ++c)
                        p[c] = uint8_t(std::round(std::clamp(v[c], 0.f, 255.f)));
                } else if (colorSpace == 2) {
                    __m128 r, g, b;
                    perceptualColor::inverse4(_mm_set1_ps(yy), _mm_set1_ps(cb), _mm_set1_ps(cr), r, g, b);
                    p[0] = uint8_t(_mm_cvtsi128_si32(toInt(r)));
                    p[1] = uint8_t(_mm_cvtsi128_si32(toInt(g)));
                    p[2] = uint8_t(_mm_cvtsi128_si32(toInt(b)));
                } else {
                    p[0] = uint8_t(std::round(std::clamp(colorSpace ? yy + cb - cr : yy + 1.402f * cr, 0.f, 255.f)));
                    p[1] = uint8_t(
                        std::round(std::clamp(colorSpace ? yy + cr : yy - .344136f * cb - .714136f * cr, 0.f, 255.f)));
                    p[2] = uint8_t(std::round(std::clamp(colorSpace ? yy - cb - cr : yy + 1.772f * cb, 0.f, 255.f)));
                }
                p[3] = 255;
                if (base)
                    for (int c = 0; c < 3; ++c)
                        p[c] = uint8_t(std::clamp(int(base[x * 4 + c]) + 2 * (int(p[c]) - 128), 0, 255));
            }
            if (!flatMap.empty())
                for (uint32_t bx = 0; bx < width; bx += flatTile) {
                    const uint32_t n = std::min(uint32_t(flatTile), width - bx);
                    if (bx + n <= vectorEnd)
                        continue;
                    auto index = flatMap[size_t(y / flatTile) * flatCols + bx / flatTile];
                    if (!index)
                        continue;
                    auto dst = pixels.data() + (size_t(y) * width + bx) * 4;
                    auto rgba = _mm_set1_epi32(int(flatPalette[index]));
                    uint32_t i = 0;
                    for (; i + 4 <= n; i += 4)
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i * 4), rgba);
                    for (; i < n; ++i)
                        std::memcpy(dst + i * 4, &flatPalette[index], 4);
                }
        }
    });
    timing.mark("decode.color_restoration");
    if (opaque)
        require(storage[16].empty(), "Opaque wavelet has an alpha stream.");
    else
        undoPredict(storage[16], pixels, width, height, 3);
    timing.mark("decode.alpha");
    if (rgbRestoration) {
        restoreRgb(pixels, width, height, threads, rgbRestoration);
        timing.mark("decode.r12_joint_rgb_restoration");
    }
    return frame;
}
Frame decodeWavelet(PacketView packet, uint32_t width, uint32_t height, uint32_t threads,
                    std::span<uint8_t> destination, uint32_t smoothing, uint32_t sharpening,
                    std::span<const uint8_t> predictor, std::vector<float>* lumaOutput, bool lumaOnly,
                    std::span<const uint8_t> cflMap, uint32_t cflTile, bool cflCentered, uint8_t chromaModel, uint32_t nativeFilterStrength) {
    return decodeWaveletImpl<false>(packet, width, height, threads, destination, smoothing, sharpening, predictor,
                                    lumaOutput, lumaOnly, cflMap, cflTile, cflCentered, chromaModel, nullptr, nativeFilterStrength);
}
Frame inspectWavelet(PacketView packet, uint32_t width, uint32_t height, uint32_t threads, WaveletDebugCapture& capture,
                     std::span<const uint8_t> map, uint32_t tile) {
    require(std::isfinite(capture.gain) && capture.gain >= 1 && capture.gain <= 16, "Invalid debug gain.");
    return decodeWaveletImpl<true>(packet, width, height, threads, {}, map.empty() ? 100 : 0, 0, {}, nullptr, false,
                                   map, tile, true, 0, &capture);
}
} // namespace fajk
