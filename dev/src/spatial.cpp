#include "spatial.hpp"
#include "adaptive_allocation.hpp"
#include "binary_range.hpp"
#include "fajk/entropy.hpp"
#include <array>
#include <bit>
#include <immintrin.h>
#include <limits>
#include <type_traits>

namespace fajk {
bool hasAvx2();
bool waveletQuantize16Avx2(const float*, float, int*, float = .35f);
bool spatialRateCandidateAvx2(const float*, const float*, const float*, int, int, float, float, int*, float*, double&);
void spatialRateCoupling8Avx2(const float*,int,const float*,double&);
namespace {
using Encoder = binaryRange::Encoder;
// A private pilot needs the same adaptive prices, not a discarded byte stream.
// A separate encoder instantiation keeps this choice out of production bit loops.
struct ModelTrainer {
    unsigned adaptation = 5;
    void bit(uint16_t& probability, bool one) {
        if (one)
            probability -= probability >> adaptation;
        else
            probability += (binaryRange::probabilityScale - probability) >> adaptation;
    }
    void raw(bool) {}
    double positionBits() const { return 0; }
};
struct Decoder : binaryRange::Decoder {
    explicit Decoder(std::span<const uint8_t> bytes, bool enhanced = false, bool extended = false, bool parent = false,
                     uint8_t mode = 0)
        : binaryRange::Decoder(bytes,
                               mode == 25 && !bytes.empty() && bytes.front() == 6 ? 6
                               : mode == 26 && !bytes.empty() && bytes.front() == 7 ? 7
                               : mode                                             ? 4
                               : parent                                           ? 3
                               : extended                                         ? 2
                               : enhanced                                         ? 1
                                                                                  : 0,
                               spatialAdaptation(mode)) {}
};
// Separate contexts describe empty groups, coefficient significance,
// magnitude and sign. Only already decoded neighbors may select a context.
struct Models {
    std::array<uint16_t, 672> p;
    explicit Models(uint8_t mode = 0) {
        p.fill(2048);
        if (mode >= 23)
            for (unsigned c = 0; c < 64; ++c)
                p[8 + c] = std::popcount(c & 15) == 0 ? 3900 : std::popcount(c & 15) == 1 ? 3000 : 2048;
        if (mode == 6 || mode == 7 || mode == 10 || mode == 11) {
            const bool strong = mode == 7;
            for (unsigned c = 0; c < 16; ++c)
                p[8 + c] =
                    std::array<uint16_t, 5>{uint16_t(strong ? 4000 : 3840), 3276, 2662, 2048, 1638}[std::popcount(c)];
            for (unsigned c = 0; c < 4; ++c)
                for (unsigned start : {216u, 220u, 224u})
                    p[start + c] = c == 0 ? uint16_t(strong ? 3900 : 3500) : c == 3 ? 1600 : 2400;
        }
    }
};
// Bounded, deterministic within-band dictionary. References contain already
// quantized coefficients; similar groups transmit their integer differences.
// No additional approximation or pixel copying is involved.
struct PatternDictionary {
    struct Entry {
        std::array<int, 16> values{};
        int width = 0, height = 0;
    };
    std::array<Entry, 8> entries{};
    unsigned size = 0, next = 0;
    void remember(const Entry& entry) {
        entries[next] = entry;
        next = (next + 1) % unsigned(entries.size());
        size = std::min(unsigned(entries.size()), size + 1);
    }
};
// Cross-tile contexts read only previously coded residual coefficients.
// The private packet prefix explicitly separates this model from R16.
constexpr int contextStride = 18;
template <class T> bool parentGroup(const T* parent, int stride, int x, int y, int width, int height) {
    if (!parent)
        return false;
    for (int yy = y / 2; yy <= (y + height - 1) / 2; ++yy)
        for (int xx = x / 2; xx <= (x + width - 1) / 2; ++xx)
            if (parent[size_t(yy) * stride + xx] != 0)
                return true;
    return false;
}
void loadHalo(int* known, std::span<const int> history, int width, int height, int tx, int ty) {
    if (history.empty())
        return;
    if (ty)
        for (int x = -1; x <= 16; ++x)
            if (tx + x >= 0 && tx + x < width)
                known[-contextStride + x] = history[size_t((ty - 1) % 17) * width + tx + x];
    if (tx)
        for (int y = 0; y < std::min(16, height - ty); ++y)
            known[y * contextStride - 1] = history[size_t((ty + y) % 17) * width + tx - 1];
}
int groupContext(const int* known, int bx, int by, int size) {
    bool left = false, top = false;
    for (int y = by; y < by + size; ++y)
        left |= known[y * contextStride + bx - 1] != 0;
    for (int x = bx; x < bx + size; ++x)
        top |= known[(by - 1) * contextStride + x] != 0;
    return left + 2 * top;
}
int powerContext(const int* known, int x, int y, int current = -1) {
    const unsigned magnitude = std::max(unsigned(std::abs(known[y * contextStride + x - 1])),
                                        unsigned(std::abs(known[(y - 1) * contextStride + x])));
    const int width = int(std::bit_width(magnitude));
    return current < 0 ? std::min(3, width) : !width ? 0 : width <= current ? 1 : width == current + 1 ? 2 : 3;
}
int refinementContext(const int* known, int x, int y, unsigned bit) {
    const unsigned left = unsigned(std::abs(known[y * contextStride + x - 1])),
                   top = unsigned(std::abs(known[(y - 1) * contextStride + x]));
    return int(((left >> bit) & 1) + 2 * ((top >> bit) & 1));
}
int context(const int* known, int x, int y, uint8_t mode = 0) {
    const int extra = (mode >= 23) ? 16 * ((std::abs(known[y * contextStride + x - 1]) > 1) +
                                           2 * (std::abs(known[(y - 1) * contextStride + x]) > 1))
                                   : 0;
    return extra + bool(known[y * contextStride + x - 1]) + 2 * bool(known[(y - 1) * contextStride + x]) +
           4 * bool(known[(y - 1) * contextStride + x - 1]) + 8 * bool(known[(y - 1) * contextStride + x + 1]);
}
int predicted(const int* values, int x, int y, int mode, int pitch = 16) {
    const int left = x ? values[y * pitch + x - 1] : 0, top = y ? values[(y - 1) * pitch + x] : 0;
    if (mode == 1)
        return left;
    if (mode == 2)
        return top;
    const int corner = x && y ? values[(y - 1) * pitch + x - 1] : 0;
    return mode == 3 ? std::clamp(left + top - corner, std::min(left, top), std::max(left, top)) : 0;
}
int signContext(const int* known, int x, int y, uint8_t mode = 0) {
    auto sign = [](int v) { return v == 0 ? 0 : v < 0 ? 1 : 2; };
    return sign(known[y * contextStride + x - 1]) + 3 * sign(known[(y - 1) * contextStride + x]) +
           ((mode >= 23) ? 9 * sign(known[(y - 1) * contextStride + x - 1]) : 0);
}
int magnitudeContext(const int* known, int x, int y) {
    return (std::abs(known[y * contextStride + x - 1]) > 1) + 2 * (std::abs(known[(y - 1) * contextStride + x]) > 1);
}

// Short context-coded base levels avoid a separate logarithmic prefix and
// refinement bit for the numerous small coefficients at low bitrates.
int baseLevelContext(const int* known, int x, int y) {
    return std::min(3, std::abs(known[y * contextStride + x - 1])) +
           4 * std::min(3, std::abs(known[(y - 1) * contextStride + x]));
}
template <class Coder>
void encodeBaseMagnitude(Coder& coder, Models& models, const int* known, int x, int y, unsigned magnitude,
                         uint8_t mode) {
    const unsigned levels = mode == 26 ? 7 : 3;
    const int context = baseLevelContext(known, x, y);
    for (unsigned level = 1; level <= levels; ++level) {
        const bool more = magnitude > level;
        coder.bit(models.p[512 + (level - 1) * 16 + context], more);
        if (!more)
            return;
    }
    const unsigned residual = magnitude - levels, power = std::bit_width(residual) - 1;
    for (unsigned bit = 0; bit <= power; ++bit)
        coder.bit(models.p[640 + bit], bit < power);
    for (unsigned bit = power; bit-- > 0;)
        coder.raw((residual >> bit) & 1);
}
unsigned decodeBaseMagnitude(Decoder& coder, Models& models, const int* known, int x, int y, uint8_t mode) {
    const unsigned levels = mode == 26 ? 7 : 3;
    const int context = baseLevelContext(known, x, y);
    for (unsigned level = 1; level <= levels; ++level)
        if (!coder.bit(models.p[512 + (level - 1) * 16 + context]))
            return level;
    unsigned power = 0;
    while (coder.bit(models.p[640 + power])) {
        require(power < 20, "Invalid base-level magnitude.");
        ++power;
    }
    unsigned residual = 1u << power;
    for (unsigned bit = power; bit-- > 0;)
        if (coder.raw())
            residual |= 1u << bit;
    return levels + residual;
}
float baseMagnitudeCost(const Models& models, const int* known, int x, int y, unsigned magnitude, uint8_t mode);

// Encoder-only RDO: the unchanged range stream transmits the selected integer
// coefficients. Estimate costs from the current real coder probabilities;
// include the option to omit an entire 4x4 group. No source data is used by decode.
const auto decisionPrices = []() {
        std::array<float, 4097> result{};
        for (unsigned p = 1; p < 4096; ++p)
            result[p] = -std::log2(float(p) / 4096.f);
        return result;
}();
float decisionCost(uint16_t probability, bool one) {
    // Every caller uses a trained Models probability in 1..4095. Keep the
    // original clamping semantics; the immutable table needs no per-bit
    // thread-safe local-static initialization guard in this hot RDO path.
    return decisionPrices[std::clamp<unsigned>(one ? 4096 - probability : probability, 1, 4095)];
}
float baseMagnitudeCost(const Models& models, const int* known, int x, int y, unsigned magnitude, uint8_t mode) {
    const unsigned levels = mode == 26 ? 7 : 3;
    const int context = baseLevelContext(known, x, y);
    float result = 0;
    for (unsigned level = 1; level <= levels; ++level) {
        const bool more = magnitude > level;
        result += decisionCost(models.p[512 + (level - 1) * 16 + context], more);
        if (!more)
            return result;
    }
    const unsigned power = std::bit_width(magnitude - levels) - 1;
    for (unsigned bit = 0; bit <= power; ++bit)
        result += decisionCost(models.p[640 + bit], bit < power);
    return result + power;
}
template<bool current = false>
float coefficientCost(const Models& models, const int* known, int x, int y, int value, bool enhanced,
                      uint8_t entropyMode = 0) {
    if constexpr(current){entropyMode=32;enhanced=true;}
    float cost = decisionCost(models.p[8 + context(known, x, y, entropyMode)], value != 0);
    if (!value)
        return cost;
    const unsigned magnitude = unsigned(std::abs(value)), power = std::bit_width(magnitude) - 1;
    const int c = magnitudeContext(known, x, y);
    if (entropyMode == 25 || entropyMode == 26 || entropyMode == 29 || entropyMode == 30)
        return cost + baseMagnitudeCost(models, known, x, y, magnitude, entropyMode) +
               decisionCost(models.p[128 + signContext(known, x, y, entropyMode)], value < 0);
    for (unsigned bit = 0; bit <= power; ++bit)
        cost += decisionCost(models.p[(enhanced ? 256 : 32) + bit * 4 + (enhanced ? powerContext(known, x, y) : c)],
                             bit < power);
    for (unsigned bit = power; bit-- > 0;)
        cost += decisionCost(models.p[enhanced ? 352 + std::min(7u, bit) * 4 + refinementContext(known, x, y, bit)
                                               : 120 + std::min(7u, bit)],
                             (magnitude >> bit) & 1);
    return cost + decisionCost(models.p[128 + signContext(known, x, y, entropyMode)], value < 0);
}
template<bool current = false>
struct CoefficientPriceCache {
    const Models& models;
    const int* known;
    int x,y;
    bool enhanced;
    uint8_t mode;
    unsigned left=0,top=0,powerIndex=0,signIndex=0,filled=0;
    float zero=0,prefix=0,terminal[32];
    CoefficientPriceCache(const Models& m,const int* k,int xx,int yy,bool e,uint8_t md)
        :models(m),known(k),x(xx),y(yy),enhanced(e),mode(md){
        if constexpr(current){
            const auto p=models.p[8+context(known,x,y,32)];
            zero=decisionCost(p,false);prefix=decisionCost(p,true);
            left=unsigned(std::abs(known[y*contextStride+x-1]));
            top=unsigned(std::abs(known[(y-1)*contextStride+x]));
            powerIndex=256+unsigned(std::min(3,std::bit_width(std::max(left,top))));
            signIndex=128+signContext(known,x,y,32);
        }
    }
    float operator()(int value){
        if constexpr(!current)return coefficientCost(models,known,x,y,value,enhanced,mode);
        else{
            if(!value)return zero;
            const unsigned magnitude=unsigned(std::abs(value)),power=std::bit_width(magnitude)-1;
            while(filled<=power){
                const auto p=models.p[powerIndex+filled*4];
                terminal[filled]=prefix+decisionCost(p,false);
                prefix+=decisionCost(p,true);++filled;
            }
            float result=terminal[power];
            for(unsigned bit=power;bit-->0;){
                const unsigned refinement=((left>>bit)&1)+2*((top>>bit)&1);
                result+=decisionCost(models.p[352+std::min(7u,bit)*4+refinement],(magnitude>>bit)&1);
            }
            return result+decisionCost(models.p[signIndex],value<0);
        }
    }
};

template<bool current = false>
void optimizeQuantizedTile(int* values, const float* source, int stride, int width, int height, float inverse,
                           float lambda, const Models& models, std::span<const int> history, int bandWidth,
                           int bandHeight, int tx, int ty, bool enhanced, float gradient, float synthesisCoupling,
                           const float* correlations, uint8_t entropyMode,
                           const float* groupRates, int groupStride, float flatRateBoost, uint32_t rateCell) {
    if constexpr(current){entropyMode=32;enhanced=true;}
    const bool reviveEmptyGroups = gradient < 0;
    gradient = std::abs(gradient);
    int neighborhood[18 * 18]{};
    int* known = neighborhood + 19;
    loadHalo(known, history, bandWidth, bandHeight, tx, ty);
    const bool weighted = lambda < 0;
    const float baseLambda = std::abs(lambda);
    float normalized[256];
    double errors[256];
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int at = y * 16 + x;
            normalized[at] = source[size_t(y) * stride + x] * inverse;
            errors[at] = double(normalized[at]) - values[at];
        }
    for (int sy = 0; sy < height; sy += 8)
        for (int sx = 0; sx < width; sx += 8)
            for (int by = sy; by < std::min(height, sy + 8); by += 4)
                for (int bx = sx; bx < std::min(width, sx + 8); bx += 4) {
                    float groupRate = groupRates ? groupRates[size_t(by / rateCell) * groupStride + bx / rateCell] : 1.f;
                    if (groupRates && rateCell < 4) {
                        double total = 0; int count = 0;
                        for (int yy=by;yy<std::min(height,by+4);++yy)
                            for (int xx=bx;xx<std::min(width,bx+4);++xx) {
                                total += groupRates[size_t(yy/rateCell)*groupStride+xx/rateCell]; ++count;
                            }
                        groupRate = float(total/count);
                    }
                    const float lambda = baseLambda * groupRate * flatRateMultiplier(groupRate, flatRateBoost);
                    bool initiallyOccupied = false;
                    for (int y = by; y < std::min(height, by + 4); ++y)
                        for (int x = bx; x < std::min(width, bx + 4); ++x)
                            initiallyOccupied |= values[y * 16 + x] != 0;
                    if (!initiallyOccupied && !reviveEmptyGroups)
                        continue;
                    const int contextIndex = enhanced ? 220 + groupContext(known, bx, by, 4) : 2;
                    double activeCost = lambda * decisionCost(models.p[contextIndex], true),
                           emptyCost = lambda * decisionCost(models.p[contextIndex], false);
                    bool any = false;
                    for (int y = by; y < std::min(height, by + 4); ++y)
                        for (int x = bx; x < std::min(width, bx + 4); ++x) {
                            const float sourceRate = groupRates ? groupRates[size_t(y/rateCell)*groupStride+x/rateCell] : 1.f;
                            const float lambda = baseLambda * sourceRate * flatRateMultiplier(sourceRate,flatRateBoost);
                            const int initial = values[y * 16 + x];
                            int best = initial;
                            if (!initial && !gradient) {
                                activeCost +=
                                    lambda * decisionCost(models.p[8 + context(known, x, y, entropyMode)], false);
                                continue;
                            }
                            const float original = normalized[y * 16 + x],
                                        magnitude = std::abs(original);
                            const float importance = weighted ? 1 + 2 * std::min(2.f, magnitude) : 1.f;
                            double neighborErrors[4]{};
                            int neighborCount = 0;
                            if (gradient)
                                for (auto [dx, dy] :
                                     {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
                                    const int nx = x + dx, ny = y + dy;
                                    if (nx >= 0 && ny >= 0 && nx < width && ny < height)
                                        neighborErrors[neighborCount++] = errors[ny * 16 + nx];
                                }
                            double coupledError = 0;
                            if (synthesisCoupling && correlations){
                                // Same twelve terms in the original dy/dx order.
                                // Avoid visiting thirteen rejected offsets and
                                // recomputing abs/index classification per sample.
                                const int at=y*16+x;
                                if(y>=2)coupledError+=correlations[3]*errors[at-32];
                                if(y){
                                    if(x)coupledError+=correlations[4]*errors[at-17];
                                    coupledError+=correlations[1]*errors[at-16];
                                    if(x+1<width)coupledError+=correlations[4]*errors[at-15];
                                }
                                if(x>=2)coupledError+=correlations[2]*errors[at-2];
                                if(x)coupledError+=correlations[0]*errors[at-1];
                                if(x+1<width)coupledError+=correlations[0]*errors[at+1];
                                if(x+2<width)coupledError+=correlations[2]*errors[at+2];
                                if(y+1<height){
                                    if(x)coupledError+=correlations[4]*errors[at+15];
                                    coupledError+=correlations[1]*errors[at+16];
                                    if(x+1<width)coupledError+=correlations[4]*errors[at+17];
                                }
                                if(y+2<height)coupledError+=correlations[3]*errors[at+32];
                            }
                            auto distortion = [&](int q) {
                                const double error = double(original) - q;
                                double result =
                                    importance * error * error + 2 * synthesisCoupling * error * coupledError;
                                for (int neighbor = 0; neighbor < neighborCount; ++neighbor) {
                                    const double difference = error - neighborErrors[neighbor];
                                    result += gradient * difference * difference;
                                }
                                return result;
                            };
                            CoefficientPriceCache<current> prices(models,known,x,y,enhanced,entropyMode);
                            auto cost = [&](int q) {
                                return distortion(q) +
                                       lambda * prices(q);
                            };
                            double bestCost = cost(best);
                            // Coding cost is nonnegative. If distortion alone already
                            // loses, skip all probability/context work for this candidate.
                            // Keep the original arithmetic and strict tie-breaking rule.
                            auto consider = [&](int candidate) {
                                const double error = distortion(candidate);
                                if (error >= bestCost)
                                    return;
                                const double candidateCost =
                                    error +
                                    lambda * prices(candidate);
                                if (candidateCost < bestCost) {
                                    best = candidate;
                                    bestCost = candidateCost;
                                }
                            };
                            const int smaller = initial > 0 ? initial - 1 : initial + 1;
                            for (int choice = 0; choice < 2; ++choice) {
                                if (choice && smaller == 0)
                                    continue;
                                const int candidate = choice ? smaller : 0;
                                if (candidate == initial)
                                    continue;
                                consider(candidate);
                            }
                            if (gradient || synthesisCoupling) {
                                const int larger = initial ? initial + (initial > 0 ? 1 : -1) : (original < 0 ? -1 : 1);
                                consider(larger);
                            }
                            values[y * 16 + x] = known[y * contextStride + x] = best;
                            errors[y * 16 + x] = double(original) - best;
                            any |= best != 0;
                            activeCost += bestCost;
                            emptyCost += importance * double(original) * original;
                        }
                    if (!any || (!gradient && !synthesisCoupling && emptyCost < activeCost))
                        for (int y = by; y < std::min(height, by + 4); ++y)
                            for (int x = bx; x < std::min(width, bx + 4); ++x)
                            {
                                values[y * 16 + x] = known[y * contextStride + x] = 0;
                                errors[y * 16 + x] = normalized[y * 16 + x];
                            }
                }
}

bool emptyQuantizedTile(const float* source, int stride, int width, int height, float step, float rounding,
                        float minimumFactor = .5f) {
    auto maximum = _mm_setzero_ps();
    const auto sign = _mm_set1_ps(-0.f), inverse = _mm_set1_ps(1.f / step);
    float tail = 0;
    for (int y = 0; y < height; ++y) {
        int x = 0;
        for (; x + 4 <= width; x += 4)
            maximum = _mm_max_ps(
                maximum, _mm_andnot_ps(sign, _mm_mul_ps(_mm_loadu_ps(source + size_t(y) * stride + x), inverse)));
        for (; x < width; ++x)
            tail = std::max(tail, std::abs(source[size_t(y) * stride + x] / step));
    }
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, maximum);
    for (float value : lanes)
        tail = std::max(tail, value);
    // Finest permitted local step is half the band step. The margin avoids
    // skipping a coefficient at a floating-point rounding boundary.
    return tail < minimumFactor * (1 - rounding) - .000001f;
}
int fastQuantizer(const float* source, int stride, int width, int height, float step, float rounding, float rate,
                  bool extendedPrecision) {
    std::array<double, 5> costs{.04, 0., .04, .08, .08};
    const auto inverse = _mm_set1_ps(1.f / step), sign = _mm_set1_ps(-0.f), zero = _mm_setzero_ps(),
               bias = _mm_set1_ps(rounding);
    for (int y = 0; y < height; ++y) {
        int x = 0;
        for (; x + 4 <= width; x += 4) {
            const auto magnitude =
                _mm_andnot_ps(sign, _mm_mul_ps(_mm_loadu_ps(source + size_t(y) * stride + x), inverse));
            const auto importance =
                _mm_add_ps(_mm_set1_ps(1), _mm_mul_ps(_mm_set1_ps(2), _mm_min_ps(_mm_set1_ps(2), magnitude)));
            for (int candidate = 0; candidate < (extendedPrecision ? 5 : 3); ++candidate) {
                const float factor = std::array<float, 5>{.5f, 1.f, 2.f, .25f, 4.f}[candidate];
                const auto q =
                    _mm_cvtepi32_ps(_mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(magnitude, _mm_set1_ps(1 / factor)), bias)));
                const auto error = _mm_sub_ps(magnitude, _mm_mul_ps(q, _mm_set1_ps(factor)));
                // q is bounded by the wavelet coefficient limit; its float exponent
                // gives the exact integer bit width for this rate estimate.
                const auto exponent = _mm_sub_epi32(_mm_srli_epi32(_mm_castps_si128(q), 23), _mm_set1_epi32(126));
                const auto bits =
                    _mm_and_ps(_mm_cmpgt_ps(q, zero),
                               _mm_add_ps(_mm_set1_ps(3), _mm_mul_ps(_mm_set1_ps(2), _mm_cvtepi32_ps(exponent))));
                const auto cost =
                    _mm_add_ps(_mm_mul_ps(importance, _mm_mul_ps(error, error)), _mm_mul_ps(bits, _mm_set1_ps(rate)));
                alignas(16) float lanes[4];
                _mm_store_ps(lanes, cost);
                for (float value : lanes)
                    costs[candidate] += value;
            }
        }
        for (; x < width; ++x) {
            const float magnitude = std::abs(source[size_t(y) * stride + x] / step);
            for (int candidate = 0; candidate < (extendedPrecision ? 5 : 3); ++candidate) {
                const float factor = std::array<float, 5>{.5f, 1.f, 2.f, .25f, 4.f}[candidate];
                const int q = int(magnitude / factor + rounding);
                const double error = magnitude - q * factor;
                costs[candidate] += (1 + 2 * std::min(2.f, magnitude)) * error * error +
                                    rate * (q ? 3 + 2 * std::bit_width(unsigned(q)) : 0);
            }
        }
    }
    int best = 1;
    for (int candidate = 0; candidate < (extendedPrecision ? 5 : 3); ++candidate)
        if (costs[candidate] < costs[best])
            best = candidate;
    return best;
}
// Choose the tile quantizer using the actual within-band synthesis Gram matrix.
// Off-diagonal products account for coherent quantization errors along edges.
// Each pair is counted exactly once; no decoder work or metadata is added.
int coupledTileQuantizer(const float* source, int stride, int width, int height, float step, float rounding, float rate,
                         const float* correlations, uint8_t mode, bool extendedPrecision) {
    std::array<double, 5> costs{.04, 0., .04, .08, .08};
    const double coupling = mode == 1 ? 0 : mode == 4 ? .5 : 1;
    const bool weighted = mode >= 3;
    // All candidate precisions use exactly the same normalized source and
    // perceptual weights. Compute divisions once, preserving float arithmetic.
    float normalized[256], magnitudes[256], importance[256];
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        const int at=y*16+x;
        const float value=source[size_t(y)*stride+x]/step,magnitude=std::abs(value);
        normalized[at]=value;magnitudes[at]=magnitude;
        importance[at]=weighted?1+2*std::min(2.f,magnitude):1;
    }
    for (int candidate = 0; candidate < (extendedPrecision ? 5 : 3); ++candidate) {
        const float factor = std::array<float, 5>{.5f, 1.f, 2.f, .25f, 4.f}[candidate];
        float errors[256];
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const float value = normalized[y*16+x], magnitude = magnitudes[y*16+x];
                const int q = int(magnitude / factor + rounding);
                const double error = value - std::copysign(q * factor, value);
                errors[y * 16 + x] = float(error);
                costs[candidate] += importance[y*16+x] * error * error +
                                    rate * (q ? 3 + 2 * std::bit_width(unsigned(q)) : 0);
            }
        if (coupling && correlations)
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const double error = errors[y * 16 + x];
                    double neighbor = 0;
                    if (x + 1 < width)
                        neighbor += correlations[0] * errors[y * 16 + x + 1];
                    if (x + 2 < width)
                        neighbor += correlations[2] * errors[y * 16 + x + 2];
                    if (y + 1 < height) {
                        neighbor += correlations[1] * errors[(y + 1) * 16 + x];
                        if (x)
                            neighbor += correlations[4] * errors[(y + 1) * 16 + x - 1];
                        if (x + 1 < width)
                            neighbor += correlations[4] * errors[(y + 1) * 16 + x + 1];
                    }
                    if (y + 2 < height)
                        neighbor += correlations[3] * errors[(y + 2) * 16 + x];
                    costs[candidate] += 2 * coupling * error * neighbor;
                }
    }
    int best = 1;
    for (int candidate = 0; candidate < (extendedPrecision ? 5 : 3); ++candidate)
        if (costs[candidate] < costs[best])
            best = candidate;
    return best;
}
// Quantizer allocation uses current range-model prices, including empty groups
// and local precision flags, rather than integer-width estimates alone.
template<bool current = false>
int actualRateQuantizer(const float* source, int stride, int width, int height, float step, float rounding, float rate,
                        const Models& models, std::span<const int> history, int bandWidth, int bandHeight, int tx,
                        int ty, int tileContext, const float* correlations, uint8_t entropyMode,
                        float sourcePrecisionPriority = 1, uint8_t restrictedPrecision = 0) {
    if constexpr(current)entropyMode=32;
    const bool extendedPrecision = (entropyMode == 24 || entropyMode == 32);
    double costs[5]{};
    // The local precisions evaluate the same normalized source. Preserve
    // the original float divisions and double cost order, but do them once.
    float normalized[256], magnitudes[256], importance[256];
    const auto divisor = _mm_set1_ps(step), sign = _mm_set1_ps(-0.f), one = _mm_set1_ps(1.f), two = _mm_set1_ps(2.f),
               bias = _mm_set1_ps(rounding);
    for (int y = 0; y < height; ++y) {
        int x = 0;
        for (; x + 4 <= width; x += 4) {
            const int at = y * 16 + x;
            const auto value = _mm_div_ps(_mm_loadu_ps(source + size_t(y) * stride + x), divisor),
                       magnitude = _mm_andnot_ps(sign, value);
            _mm_storeu_ps(normalized + at, value);
            _mm_storeu_ps(magnitudes + at, magnitude);
            _mm_storeu_ps(importance + at, _mm_add_ps(one, _mm_mul_ps(two, _mm_min_ps(two, magnitude))));
        }
        for (; x < width; ++x) {
            const int at = y * 16 + x;
            const float value = source[size_t(y) * stride + x] / step, magnitude = std::abs(value);
            normalized[at] = value;
            magnitudes[at] = magnitude;
            importance[at] = 1 + 2 * std::min(2.f, magnitude);
        }
    }
    std::array<int, 18 * 18> halo{};
    loadHalo(halo.data() + 19, history, bandWidth, bandHeight, tx, ty);
    std::array<int, 5> order{0, 1, 2, 3, 4};
    const int choices = extendedPrecision && !restrictedPrecision ? 5 : 3;
    if (extendedPrecision && restrictedPrecision == 1) {
        if (sourcePrecisionPriority < 1)
            order = {3, 0, 1, 2, 4};
        else if (sourcePrecisionPriority > 1)
            order = {1, 2, 4, 0, 3};
    }
    auto evaluationOrder = order;
    auto normal = std::find(evaluationOrder.begin(), evaluationOrder.begin() + choices, 1);
    std::rotate(evaluationOrder.begin(), normal, normal + 1);
    const bool bounded = !correlations && rate > 0;
    double bestEvaluated = std::numeric_limits<double>::infinity();
    const bool vectorCandidate = (width == 8 || width == 16) && hasAvx2();
    for (int choice = 0; choice < choices; ++choice) {
        const int candidate = evaluationOrder[choice];
        const float factor = std::array<float, 5>{.5f, 1.f, 2.f, .25f, 4.f}[candidate];
        int values[256]{};
        float errors[256]{};
        bool any = false;
        const auto spacing = _mm_set1_ps(factor);
        if (vectorCandidate)
            any = spatialRateCandidateAvx2(normalized, magnitudes, importance, width, height, factor, rounding, values, errors,
                                           costs[candidate]);
        else
            for (int y = 0; y < height; ++y) {
                int x = 0;
                for (; x + 4 <= width; x += 4) {
                    const int at = y * 16 + x;
                    const auto value = _mm_loadu_ps(normalized + at);
                    auto q = _mm_cvttps_epi32(_mm_add_ps(_mm_div_ps(_mm_loadu_ps(magnitudes + at), spacing), bias));
                    const auto negative = _mm_castps_si128(_mm_cmplt_ps(value, _mm_setzero_ps()));
                    q = _mm_sub_epi32(_mm_xor_si128(q, negative), negative);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(values + at), q);
                    any |= _mm_movemask_epi8(_mm_cmpeq_epi32(q, _mm_setzero_si128())) != 65535;
                    _mm_storeu_ps(errors + at, _mm_sub_ps(value, _mm_mul_ps(_mm_cvtepi32_ps(q), spacing)));
                    for (int lane = 0; lane < 4; ++lane) {
                        const double error = errors[at + lane];
                        costs[candidate] += importance[at + lane] * error * error;
                    }
                }
                for (; x < width; ++x) {
                    const int at = y * 16 + x;
                    const float value = normalized[at], magnitude = magnitudes[at];
                    const int q = int(magnitude / factor + rounding);
                    values[at] = value < 0 ? -q : q;
                    any |= q != 0;
                    const double error = value - values[at] * factor;
                    errors[at] = float(error);
                    costs[candidate] += importance[at] * error * error;
                }
            }
        if(correlations&&width==8&&hasAvx2())
            spatialRateCoupling8Avx2(errors,height,correlations,costs[candidate]);
        else if (correlations)
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    double neighbor = 0;
                    if (x + 1 < width)
                        neighbor += correlations[0] * errors[y * 16 + x + 1];
                    if (x + 2 < width)
                        neighbor += correlations[2] * errors[y * 16 + x + 2];
                    if (y + 1 < height) {
                        neighbor += correlations[1] * errors[(y + 1) * 16 + x];
                        if (x)
                            neighbor += correlations[4] * errors[(y + 1) * 16 + x - 1];
                        if (x + 1 < width)
                            neighbor += correlations[4] * errors[(y + 1) * 16 + x + 1];
                    }
                    if (y + 2 < height)
                        neighbor += correlations[3] * errors[(y + 2) * 16 + x];
                    costs[candidate] += 2 * errors[y * 16 + x] * neighbor;
                }
        // Distortion and completed group prices are nonnegative when there is
        // no Gram coupling. Stop only after a candidate cannot beat a fully
        // evaluated one; keep the original tie selection below.
        if (bounded && costs[candidate] >= bestEvaluated) {
            costs[candidate] = std::numeric_limits<double>::infinity();
            continue;
        }
        auto price = [&]() {
            double bits = decisionCost(models.p[tileContext], any);
            if (any) {
                bits += decisionCost(models.p[204], candidate != 1);
                if (candidate != 1) {
                    bits += decisionCost(models.p[205], candidate == 2 || candidate == 4);
                    if (extendedPrecision)
                        bits += decisionCost(models.p[206], candidate >= 3);
                }
                auto neighborhood = halo;
                int* known = neighborhood.data() + 19;
                // Reuse exact occupancy for 8x8 and 4x4 groups. The coefficient
                // array is zero-padded, so this also covers partial boundary tiles.
                unsigned occupied = 0;
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; x += 4) {
                        const auto v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(values + y * 16 + x));
                        if (_mm_movemask_epi8(_mm_cmpeq_epi32(v, _mm_setzero_si128())) != 65535)
                            occupied |= 1u << ((y / 4) * 4 + x / 4);
                    }
                bool tileChild = false;
                for (int sy = 0; sy < height; sy += 8)
                    for (int sx = 0; sx < width; sx += 8) {
                        const int ey = std::min(height, sy + 8), ex = std::min(width, sx + 8);
                        const bool group = (occupied & (0x33u << ((sy / 4) * 4 + sx / 4))) != 0;
                        if (!(entropyMode >= 16 && !tileChild && ey == height && ex == width))
                            bits += decisionCost(models.p[216 + groupContext(known, sx, sy, 8)], group);
                        if (!group)
                            continue;
                        tileChild = true;
                        bool groupChild = false;
                        for (int by = sy; by < ey; by += 4)
                            for (int bx = sx; bx < ex; bx += 4) {
                                const int ry = std::min(ey, by + 4), rx = std::min(ex, bx + 4);
                                const bool active = (occupied & (1u << ((by / 4) * 4 + bx / 4))) != 0;
                                if (!(entropyMode >= 16 && !groupChild && ry == ey && rx == ex))
                                    bits += decisionCost(models.p[220 + groupContext(known, bx, by, 4)], active);
                                if (!active)
                                    continue;
                                groupChild = true;
                                bool coefficientChild = false;
                                for (int y = by; y < ry; ++y)
                                    for (int x = bx; x < rx; ++x) {
                                        const int value = values[y * 16 + x];
                                        bits += coefficientCost<current>(models, known, x, y, value, true, entropyMode);
                                        if (entropyMode >= 16 && !coefficientChild && y + 1 == ry && x + 1 == rx)
                                            bits -= decisionCost(models.p[8 + context(known, x, y, entropyMode)], true);
                                        coefficientChild |= value != 0;
                                        known[y * contextStride + x] = value;
                                    }
                                if (bounded && costs[candidate] + rate * bits >= bestEvaluated)
                                    return std::numeric_limits<double>::infinity();
                            }
                    }
            }
            return bits;
        };
        costs[candidate] += rate * price();
        bestEvaluated = std::min(bestEvaluated, costs[candidate]);
    }
    int best = 1;
    for (int choice = 0; choice < choices; ++choice) {
        const int candidate = order[choice];
        if (costs[candidate] < costs[best])
            best = candidate;
    }
    return best;
}
} // namespace
std::vector<SpatialTileBound> spatialTileBounds(const float* source, int stride, int width, int height, uint8_t mode) {
    require(width > 0 && height > 0 && stride >= width, "Invalid coefficient bound dimensions.");
    const int extent = spatialTileExtent(mode), cols = (width + extent - 1) / extent;
    std::vector<SpatialTileBound> bounds(size_t(cols) * ((height + extent - 1) / extent));
    const auto sign = _mm_set1_ps(-0.f);
    for (int ty = 0; ty < height; ty += extent)
        for (int tx = 0; tx < width; tx += extent) {
            auto maximum = _mm_setzero_ps();
            float tail = 0;
            const int right = std::min(width, tx + extent), bottom = std::min(height, ty + extent);
            for (int y = ty; y < bottom; ++y) {
                const auto* row = source + size_t(y) * stride;
                int x = tx;
                for (; x + 4 <= right; x += 4)
                    maximum = _mm_max_ps(maximum, _mm_andnot_ps(sign, _mm_loadu_ps(row + x)));
                for (; x < right; ++x)
                    tail = std::max(tail, std::abs(row[x]));
            }
            alignas(16) float lanes[4];
            _mm_store_ps(lanes, maximum);
            float vectorMaximum = 0;
            for (float value : lanes)
                vectorMaximum = std::max(vectorMaximum, value);
            bounds[size_t(ty / extent) * cols + tx / extent] = {vectorMaximum, tail};
        }
    return bounds;
}
template <bool selectionOnly, bool current = false, bool uniform = false>
Bytes encodeSpatialBandImpl(const float* source, int stride, int width, int height, float step, bool prediction,
                        float rounding, bool median, float rdo, bool omitEmpty, uint8_t adaptive, bool enhanced,
                        bool extended, float adaptiveRate, const uint8_t* parent, int parentStride,
                        uint8_t* significance, int significanceStride, uint8_t entropyMode, float quantRdo,
                        float quantGradient, float synthesisCoupling, const float* correlations,
                        uint8_t coupledQuantizer, bool rateQuantizer, uint8_t patternSimilarity,
                        std::span<const float> tileRates, uint8_t restrictedPrecision,
                        std::span<const SpatialTileBound> bounds, std::vector<float>* tileCosts,
                        std::vector<float>* selectedIntegers, std::span<const float> groupRates, std::vector<float>* tilePrecisions, float flatRateBoost, uint32_t rateCell) {
    require(std::isfinite(flatRateBoost) && flatRateBoost >= 1 && flatRateBoost <= 4096, "Invalid flat rate boost.");
    require(restrictedPrecision <= 2 && (!restrictedPrecision || (entropyMode == 24 || entropyMode == 32)),
            "Invalid source precision restriction.");
    require((entropyMode != 24 && entropyMode != 32) || (adaptive == 3 && enhanced && extended && !parent),
            "Invalid extended precision quantizer.");
    require(!rateQuantizer || (enhanced && extended && adaptive == 3 && !parent), "Invalid rate quantizer.");
    require(coupledQuantizer <= 4, "Invalid synthesis quantizer.");
    require(patternSimilarity <= 100, "Invalid pattern similarity.");
    SpatialCoderConfig{entropyMode, enhanced, extended, parent != nullptr, parentStride}.validate(width, height, stride,
                                                                                                  step);
    require(source && adaptive <= 3 && (!significance || significanceStride >= width),
            "Invalid adaptive coefficient quantizer.");
    require(!selectedIntegers || adaptive == 0 || adaptive == 3, "Invalid reconstruction capture mode.");
    if (selectedIntegers)
        selectedIntegers->assign(size_t(width) * height, 0.f);
    // The R470 model has fixed invariants; specialize only after validating
    // inputs so the compiler can remove retired per-coefficient branches.
    if constexpr(current){entropyMode=32;adaptive=uniform?0:3;enhanced=true;extended=true;parent=nullptr;}
    using Coder = std::conditional_t<selectionOnly, ModelTrainer, Encoder>;
    Coder coder;
    coder.adaptation = spatialAdaptation(entropyMode);
    Models models(entropyMode);
    bool occupiedBand = false;
    const auto bias = _mm_set1_ps(rounding), sign = _mm_set1_ps(-0.f);
    const bool avx = hasAvx2();
    const int tileExtent = spatialTileExtent(entropyMode), tileCols = (width + tileExtent - 1) / tileExtent;
    std::vector<int> history(extended ? size_t(width) * 17 : 0);
    Bytes occupiedTiles(extended ? size_t(tileCols) * ((height + tileExtent - 1) / tileExtent) : 0);
    PatternDictionary patterns;
    require(tileRates.empty() || tileRates.size() == size_t(tileCols) * ((height + tileExtent - 1) / tileExtent),
            "Invalid source priority grid.");
    require(bounds.empty() || bounds.size() == size_t(tileCols) * ((height + tileExtent - 1) / tileExtent),
            "Invalid coefficient bound grid.");
    if (tilePrecisions)
        tilePrecisions->assign(size_t(tileCols) * ((height + tileExtent - 1) / tileExtent), 0);
    if (tileCosts)
        tileCosts->assign(size_t(tileCols) * ((height + tileExtent - 1) / tileExtent), 0);
    for (float value : tileRates)
        require(std::isfinite(value) && value >= .03125f && value <= 128.f, "Invalid source priority weight.");
    require(rateCell == 1 || rateCell == 2 || rateCell == 4, "Invalid RDO price footprint.");
    const int groupCols = (width + rateCell - 1) / rateCell;
    require(groupRates.empty() || ((adaptive == 0 || adaptive == 3) && (!prediction || entropyMode == 32) && !parent &&
                groupRates.size() == size_t(groupCols) * ((height + rateCell - 1) / rateCell)), "Invalid RDO group rate grid.");
    for (float value : groupRates)
        require(std::isfinite(value) && value >= .03125f && value <= 128.f, "Invalid RDO group rate.");
    // An omitted all-zero band has no coded occupancy history. Reuse the
    // conservative source bounds before visiting its tiles on each rate pass.
    // Significance outputs still need their normal clearing path.
    if (omitEmpty && !significance && !bounds.empty() && adaptive == 0 && quantGradient >= 0) {
        const float threshold = (1 - rounding) - .000001f, reciprocal = 1.f / step;
        if (std::all_of(bounds.begin(), bounds.end(), [&](const auto& b) {
                return std::max(b.vectorMaximum * reciprocal, b.tailMaximum / step) < threshold;
            }))
            return {};
    }
    for (int ty = 0; ty < height; ty += tileExtent)
        for (int tx = 0; tx < width; tx += tileExtent) {
            SpatialTileTimer tileTimer;
            if (extended && tx == 0)
                for (int y = ty; y < std::min(height, ty + tileExtent); ++y)
                    std::fill_n(history.data() + size_t(y % 17) * width, width, 0);
            const size_t tileAt = size_t(ty / tileExtent) * tileCols + tx / tileExtent;
            struct CostCapture {
                Coder& coder;
                float* target;
                double start;
                ~CostCapture() {
                    if (target)
                        *target = float(std::max(0., coder.positionBits() - start));
                }
            } capture{coder, tileCosts ? &(*tileCosts)[tileAt] : nullptr, tileCosts ? coder.positionBits() : 0.};
            const float priority = tileRates.empty() ? 1.f : tileRates[tileAt];
            const float localRate = adaptiveRate * priority * flatRateMultiplier(priority, flatRateBoost);
            int values[256]{};
            const int w = std::min(tileExtent, width - tx), h = std::min(tileExtent, height - ty);
            bool any = false;
            const int tileContext = extended ? (parent ? 448 : 224) + (tx ? bool(occupiedTiles[tileAt - 1]) : 0) +
                                                   2 * (ty ? bool(occupiedTiles[tileAt - tileCols]) : 0) +
                                                   (parent ? 4 * parentGroup(parent, parentStride, tx, ty, w, h) : 0)
                                             : 0;
            if (adaptive == 0 || adaptive == 3) {
                const float minimumFactor = adaptive == 0 ? 1.f : (entropyMode == 24 || entropyMode == 32) ? .25f : .5f;
                // Multiplication/division by a positive step is monotonic. Keep the
                // vector reciprocal and scalar tail division distinct, as before.
                const bool empty = bounds.empty() ? emptyQuantizedTile(source + size_t(ty) * stride + tx, stride, w, h,
                                                                       step, rounding, minimumFactor)
                                                  : std::max(bounds[tileAt].vectorMaximum * (1.f / step),
                                                             bounds[tileAt].tailMaximum / step) <
                                                        minimumFactor * (1 - rounding) - .000001f;
                if (empty && quantGradient >= 0) {
                    coder.bit(models.p[tileContext], false);
                    continue;
                }
            }
            tileTimer.mark(1);
            int quantizer = 1;
            if (adaptive && restrictedPrecision == 2)
                quantizer = 1;
            else if (rateQuantizer && !prediction)
                quantizer = actualRateQuantizer<current>(source + size_t(ty) * stride + tx, stride, w, h, step, rounding,
                                                localRate, models, history, width, height, tx, ty, tileContext,
                                                correlations, entropyMode, priority, restrictedPrecision);
            else if (adaptive == 3)
                quantizer = coupledQuantizer
                                ? coupledTileQuantizer(source + size_t(ty) * stride + tx, stride, w, h, step, rounding,
                                                       localRate, correlations, coupledQuantizer, (entropyMode == 24 || entropyMode == 32))
                                : fastQuantizer(source + size_t(ty) * stride + tx, stride, w, h, step, rounding,
                                                localRate, (entropyMode == 24 || entropyMode == 32));
            else if (adaptive) {
                double best = 1e100;
                for (int candidate : {1, 0, 2}) {
                    const float factor = candidate == 0 ? .5f : candidate == 2 ? 2.f : 1.f;
                    double cost = candidate == 1 ? 0 : adaptive == 1 ? .04 : .16;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) {
                            const float value = source[size_t(ty + y) * stride + tx + x] / step,
                                        magnitude = std::abs(value);
                            const int q = int(magnitude / factor + rounding);
                            const double error = magnitude - q * factor;
                            const double importance = 1 + 2 * std::min(2.f, magnitude);
                            const double bits = q ? 3 + 2 * std::bit_width(unsigned(q)) : 0;
                            cost += importance * error * error + (adaptive == 1 ? .02 : .08) * bits;
                        }
                    if (cost < best) {
                        best = cost;
                        quantizer = candidate;
                    }
                }
            }
            tileTimer.mark(2);
            const float inverse = 1 / (step * std::array<float, 5>{.5f, 1.f, 2.f, .25f, 4.f}[quantizer]);
            const auto scale = _mm_set1_ps(inverse);
            for (int y = 0; y < h; ++y) {
                const auto* row = source + size_t(ty + y) * stride + tx;
                int x = 0;
                if (avx && w == 16) {
                    any |= waveletQuantize16Avx2(row, inverse, values + y * 16, rounding);
                    continue;
                }
                for (; x + 4 <= w; x += 4) {
                    const auto v = _mm_mul_ps(_mm_loadu_ps(row + x), scale);
                    auto q = _mm_cvttps_epi32(_mm_add_ps(_mm_andnot_ps(sign, v), bias));
                    const auto negative = _mm_srai_epi32(_mm_castps_si128(v), 31);
                    q = _mm_sub_epi32(_mm_xor_si128(q, negative), negative);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(values + y * 16 + x), q);
                    any |= _mm_movemask_epi8(_mm_cmpeq_epi32(q, _mm_setzero_si128())) != 65535;
                }
                for (; x < w; ++x) {
                    const float v = row[x] * inverse;
                    const int n = int(std::abs(v) + rounding);
                    values[y * 16 + x] = v < 0 ? -n : n;
                    any |= n != 0;
                }
            }
            if (rdo < 0) {
                any = false;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const int i = y * 16 + x, q = values[i];
                        if (!q)
                            continue;
                        if (std::abs(q) > 1) {
                            any = true;
                            continue;
                        }
                        const int gx = tx + x, gy = ty + y;
                        const float center = source[size_t(gy) * stride + gx];
                        float support = 0;
                        auto pair = [&](int dx, int dy) {
                            const int ax = gx - dx, ay = gy - dy, bx = gx + dx, by = gy + dy;
                            if (ax < 0 || ay < 0 || bx < 0 || by < 0 || ax >= width || bx >= width || ay >= height ||
                                by >= height)
                                return;
                            const float a = source[size_t(ay) * stride + ax], b = source[size_t(by) * stride + bx];
                            if (a * center > 0 && b * center > 0)
                                support = std::max(support, std::min(std::abs(a), std::abs(b)) * inverse);
                        };
                        pair(1, 0);
                        pair(0, 1);
                        pair(1, 1);
                        pair(1, -1);
                        const float magnitude = std::abs(center) * inverse;
                        const float threshold = .5f + (-rdo) / (1 + 4 * support);
                        if (magnitude < threshold)
                            values[i] = 0;
                        else
                            any = true;
                    }
            }
            if (rdo > 0) {
                any = false;
                int original[256];
                std::copy_n(values, 256, original);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const int i = y * 16 + x, q = original[i];
                        if (!q)
                            continue;
                        const int neighbors = (x && original[i - 1] != 0) + (x + 1 < w && original[i + 1] != 0) +
                                              (y && original[i - 16] != 0) + (y + 1 < h && original[i + 16] != 0);
                        const float v = std::abs(source[size_t(ty + y) * stride + tx + x]) * inverse,
                                    reconstructed = std::abs(q) + .15f;
                        const float benefit = v * v - (v - reconstructed) * (v - reconstructed);
                        const float cost =
                            rdo * (3.f + 2.f * std::bit_width(unsigned(std::abs(q)))) / (1.f + .35f * neighbors);
                        if (benefit < cost)
                            values[i] = 0;
                        else
                            any = true;
                    }
            }
            tileTimer.mark(3);
            if (quantRdo && (!prediction || (entropyMode == 32 && flatRateBoost > 1)) && !parent) {
                // Candidate distortion is measured in local quantizer units.
                // Keep the base-step rate/distortion tradeoff invariant when
                // the preceding local precision decision changes that unit.
                const float precision = std::array<float,5>{.5f,1.f,2.f,.25f,4.f}[quantizer];
                const float rateScale = entropyMode == 32 ? 1.f / (precision * precision) : 1.f;
                optimizeQuantizedTile<current>(values, source + size_t(ty) * stride + tx, stride, w, h, inverse,
                                      quantRdo * rateScale * (groupRates.empty() ? priority * flatRateMultiplier(priority, flatRateBoost) : 1.f), models, history, width, height, tx, ty, enhanced,
                                      quantGradient, synthesisCoupling, correlations, entropyMode,
                                      groupRates.empty() ? nullptr : groupRates.data() + size_t(ty / rateCell) * groupCols + tx / rateCell,
                                      groupCols, groupRates.empty() ? 1.f : flatRateBoost, rateCell);
                any = false;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        any |= values[y * 16 + x] != 0;
            }
            tileTimer.mark(4);
            if (tilePrecisions && any)
                (*tilePrecisions)[tileAt] = std::array<float,5>{.5f,1.f,2.f,.25f,4.f}[quantizer];
            if (significance)
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        significance[size_t(ty + y) * significanceStride + tx + x] = uint8_t(values[y * 16 + x] != 0);
            coder.bit(models.p[tileContext], any);
            if (!any)
                continue;
            // Retain the chosen integers before reversible prediction. The
            // alternative entropy model can consume them without decoding the
            // stream we have just encoded. Quantization/RDO runs only once.
            if (selectedIntegers)
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        (*selectedIntegers)[size_t(ty + y) * width + tx + x] = float(values[y * 16 + x]) * std::array<float,5>{.5f,1.f,2.f,.25f,4.f}[quantizer];
            if (extended)
                occupiedTiles[tileAt] = 1;
            if (adaptive) {
                coder.bit(models.p[204], quantizer != 1);
                if (quantizer != 1) {
                    coder.bit(models.p[205], quantizer == 2 || quantizer == 4);
                    if ((entropyMode == 24 || entropyMode == 32))
                        coder.bit(models.p[206], quantizer >= 3);
                }
            }
            occupiedBand = true;
            int neighborhood[18 * 18]{};
            int* known = neighborhood + 19;
            loadHalo(known, history, width, height, tx, ty);
            // Intra prediction is lossless after quantization. Evaluate only two
            // neighboring directions, once, without transform or distortion search.
            if (prediction) {
                unsigned costs[4]{};
                const int modes = median ? 4 : 3;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        for (int m = 0; m < modes; ++m) {
                            const unsigned a = unsigned(std::abs(values[y * 16 + x] - predicted(values, x, y, m)));
                            costs[m] += a ? 3 + 2 * std::bit_width(a) : 0;
                        }
                int mode = 0;
                for (int m = 1; m < modes; ++m)
                    if (costs[m] + 8 < costs[mode])
                        mode = m;
                coder.bit(models.p[200], mode != 0);
                if (mode) {
                    coder.bit(models.p[201], mode >= 2);
                    if (median && mode >= 2)
                        coder.bit(models.p[202], mode == 3);
                }
                if (mode)
                    for (int y = h - 1; y >= 0; --y)
                        for (int x = w - 1; x >= 0; --x)
                            values[y * 16 + x] -= predicted(values, x, y, mode);
            }
            // Compute occupancy once, instead of rescanning coefficients for both
            // 8x8 and 4x4 groups. Partial tiles are zero-padded in values above.
            unsigned occupied = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; x += 4) {
                    const auto v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(values + y * 16 + x));
                    if (_mm_movemask_epi8(_mm_cmpeq_epi32(v, _mm_setzero_si128())) != 65535)
                        occupied |= 1u << ((y / 4) * 4 + x / 4);
                }
            // An occupied parent implies that its last child is occupied when
            // all preceding children were empty. This removes redundant decisions
            // without changing any quantized coefficient or reconstruction.
            bool tileChild = false;
            for (int sy = 0; sy < h; sy += 8)
                for (int sx = 0; sx < w; sx += 8) {
                    const int ey = std::min(h, sy + 8), ex = std::min(w, sx + 8);
                    const bool group = (occupied & (0x33u << ((sy / 4) * 4 + sx / 4))) != 0;
                    const bool inferGroup = entropyMode >= 13 && !prediction && !tileChild && ey == h && ex == w;
                    if (!inferGroup)
                        coder.bit(models.p[parent ? 432 + groupContext(known, sx, sy, 8) +
                                                        4 * parentGroup(parent, parentStride, tx + sx, ty + sy, ex - sx,
                                                                        ey - sy)
                                           : enhanced ? 216 + groupContext(known, sx, sy, 8)
                                                      : 1],
                                  group);
                    else
                        require(group, "Invalid occupied tile inference.");
                    if (!group)
                        continue;
                    tileChild = true;
                    bool groupChild = false;
                    for (int by = sy; by < ey; by += 4)
                        for (int bx = sx; bx < ex; bx += 4) {
                            const int ry = std::min(ey, by + 4), rx = std::min(ex, bx + 4);
                            const bool active = (occupied & (1u << ((by / 4) * 4 + bx / 4))) != 0;
                            const bool inferActive = entropyMode >= 13 && !groupChild && ry == ey && rx == ex;
                            if (!inferActive)
                                coder.bit(models.p[parent ? 440 + groupContext(known, bx, by, 4) +
                                                                4 * parentGroup(parent, parentStride, tx + bx, ty + by,
                                                                                rx - bx, ry - by)
                                                   : enhanced ? 220 + groupContext(known, bx, by, 4)
                                                              : 2],
                                          active);
                            else
                                require(active, "Invalid occupied group inference.");
                            if (!active)
                                continue;
                            groupChild = true;
                            bool coefficientChild = false;
                            PatternDictionary::Entry original;
                            original.width = rx - bx;
                            original.height = ry - by;
                            if ((entropyMode == 17 || entropyMode == 18))
                                for (int y = by; y < ry; ++y)
                                    for (int x = bx; x < rx; ++x)
                                        original.values[(y - by) * 4 + x - bx] = values[y * 16 + x];
                            int reference = -1;
                            bool deltaAny = true;
                            std::array<int, 16> referenceValues{};
                            if ((entropyMode == 17 || entropyMode == 18) && patterns.size) {
                                auto price = [&](const std::array<int, 16>* pattern) {
                                    int scratch[18 * 18];
                                    std::copy_n(neighborhood, 18 * 18, scratch);
                                    int* trial = scratch + 19;
                                    float cost = 0;
                                    bool anyValue = false;
                                    for (int y = by; y < ry; ++y)
                                        for (int x = bx; x < rx; ++x) {
                                            const int at = (y - by) * 4 + x - bx,
                                                      value = original.values[at] - (pattern ? (*pattern)[at] : 0);
                                            cost += coefficientCost(models, trial, x, y, value, true);
                                            if (!anyValue && y + 1 == ry && x + 1 == rx)
                                                cost -= decisionCost(models.p[8 + context(trial, x, y)], value != 0);
                                            anyValue |= value != 0;
                                            trial[y * contextStride + x] = value;
                                        }
                                    return cost;
                                };
                                float bestCost = decisionCost(models.p[488], false) + price(nullptr);
                                for (unsigned candidate = 0; candidate < patterns.size; ++candidate) {
                                    const auto& pattern = patterns.entries[candidate];
                                    if (pattern.width != original.width || pattern.height != original.height)
                                        continue;
                                    bool different = false;
                                    uint64_t difference = 0, magnitude = 0;
                                    for (unsigned i = 0; i < 16; ++i) {
                                        different |= original.values[i] != pattern.values[i];
                                        difference += std::abs(original.values[i] - pattern.values[i]);
                                        magnitude +=
                                            std::max(std::abs(original.values[i]), std::abs(pattern.values[i]));
                                    }
                                    if (entropyMode == 17 && different)
                                        continue;
                                    if (patternSimilarity &&
                                        difference * 100 > uint64_t(100 - patternSimilarity) * magnitude)
                                        continue;
                                    const float cost = decisionCost(models.p[488], true) +
                                                       std::bit_width(patterns.size - 1) +
                                                       decisionCost(models.p[489], different) +
                                                       (different ? price(&pattern.values) : 0);
                                    if (cost + .25f < bestCost) {
                                        bestCost = cost;
                                        reference = int(candidate);
                                        deltaAny = different;
                                    }
                                }
                                coder.bit(models.p[488], reference >= 0);
                                if (reference >= 0) {
                                    referenceValues = patterns.entries[reference].values;
                                    for (unsigned b = std::bit_width(patterns.size - 1); b-- > 0;)
                                        coder.raw((unsigned(reference) >> b) & 1);
                                    coder.bit(models.p[489], deltaAny);
                                }
                            }
                            if (reference < 0 || deltaAny)
                                for (int y = by; y < ry; ++y)
                                    for (int x = bx; x < rx; ++x) {
                                        const int value = values[y * 16 + x] -
                                                          (reference >= 0 ? referenceValues[(y - by) * 4 + x - bx] : 0);
                                        const bool inferCoefficient =
                                            entropyMode >= 12 && !coefficientChild && y + 1 == ry && x + 1 == rx;
                                        const unsigned extraContext = entropyMode == 15 && !coefficientChild ? 464 : 0;
                                        if (!inferCoefficient)
                                            coder.bit(
                                                models.p[extraContext +
                                                         (parent
                                                              ? 400 + 16 * (parent[size_t((ty + y) / 2) * parentStride +
                                                                                   (tx + x) / 2] != 0)
                                                              : 8) +
                                                         context(known, x, y, entropyMode)],
                                                value != 0);
                                        else
                                            require(value != 0, "Invalid occupied coefficient inference.");
                                        if (!value)
                                            continue;
                                        coefficientChild = true;
                                        const unsigned magnitude = unsigned(std::abs(value)),
                                                       power = std::bit_width(magnitude) - 1;
                                        const int c = magnitudeContext(known, x, y);
                                        require(magnitude <= ((prediction || (entropyMode == 17 || entropyMode == 18))
                                                                  ? 2097152u
                                                                  : 1048576u),
                                                "Spatial coefficient exceeds limit.");
                                        if (entropyMode == 25 || entropyMode == 26 || entropyMode == 29 ||
                                            entropyMode == 30)
                                            encodeBaseMagnitude(coder, models, known, x, y, magnitude, entropyMode);
                                        else {
                                            const bool precise =
                                                entropyMode == 1 || entropyMode == 4 || entropyMode == 5;
                                            for (unsigned b = 0; b <= power; ++b) {
                                                const int pc =
                                                    enhanced ? powerContext(known, x, y, precise ? int(b) : -1) : c;
                                                coder.bit(models.p[(enhanced ? 256 : 32) + b * 4 + pc], b < power);
                                            }
                                            for (unsigned b = power; b-- > 0;) {
                                                const unsigned rawBits = entropyMode == 2 || entropyMode == 4   ? 1
                                                                         : entropyMode == 3 || entropyMode == 5 ? 2
                                                                                                                : 0;
                                                if (b < rawBits)
                                                    coder.raw((magnitude >> b) & 1);
                                                else
                                                    coder.bit(models.p[enhanced ? 352 + std::min(7u, b) * 4 +
                                                                                      refinementContext(known, x, y, b)
                                                                                : 120 + std::min(7u, b)],
                                                              (magnitude >> b) & 1);
                                            }
                                        }
                                        coder.bit(models.p[128 + signContext(known, x, y, entropyMode)], value < 0);
                                        known[y * contextStride + x] = value;
                                    }
                            if ((entropyMode == 17 || entropyMode == 18)) {
                                for (int y = by; y < ry; ++y)
                                    for (int x = bx; x < rx; ++x)
                                        known[y * contextStride + x] = original.values[(y - by) * 4 + x - bx];
                                patterns.remember(original);
                            }
                        }
                }
            if (extended)
                for (int y = 0; y < h; ++y)
                    std::copy_n(known + y * contextStride, w, history.data() + size_t((ty + y) % 17) * width + tx);
        }
    if (omitEmpty && !occupiedBand) {
        if (tileCosts)
            std::fill(tileCosts->begin(), tileCosts->end(), 0.f);
        return {};
    }
    if constexpr (!selectionOnly) {
        auto result = coder.finish();
        if (tileCosts) {
            double sum = 0;
            for (auto bits : *tileCosts)
                sum += bits;
            if (sum > 0)
                for (auto& bits : *tileCosts)
                    bits = float(bits * (result.size() * 8 / sum));
        }
        if (enhanced)
            result[0] = entropyMode ? 4 : parent ? 3 : extended ? 2 : 1;
        return result;
    } else {
        return {};
    }
}
Bytes encodeSpatialBand(const float* source, int stride, int width, int height, float step, bool prediction,
                        float rounding, bool median, float rdo, bool omitEmpty, uint8_t adaptive, bool enhanced,
                        bool extended, float adaptiveRate, const uint8_t* parent, int parentStride,
                        uint8_t* significance, int significanceStride, uint8_t entropyMode, float quantRdo,
                        float quantGradient, float synthesisCoupling, const float* correlations,
                        uint8_t coupledQuantizer, bool rateQuantizer, uint8_t patternSimilarity,
                        std::span<const float> tileRates, uint8_t restrictedPrecision,
                        std::span<const SpatialTileBound> bounds, std::vector<float>* tileCosts,
                        std::vector<float>* selectedIntegers, bool selectionOnly, std::span<const float> groupRates, std::vector<float>* tilePrecisions, float flatRateBoost, uint32_t rateCell) {
    require(!selectionOnly || (selectedIntegers && !tileCosts && (adaptive == 0 || adaptive == 3)),
            "Selection-only encoding requires uniform integer capture without bit costs.");
    const auto run = [&]<bool select, bool current = false, bool uniform = false>() {
        return encodeSpatialBandImpl<select, current, uniform>(source, stride, width, height, step, prediction, rounding, median, rdo, omitEmpty,
            adaptive, enhanced, extended, adaptiveRate, parent, parentStride, significance,
            significanceStride, entropyMode, quantRdo, quantGradient, synthesisCoupling,
            correlations, coupledQuantizer, rateQuantizer, patternSimilarity, tileRates,
            restrictedPrecision, bounds, tileCosts, selectedIntegers, groupRates, tilePrecisions, flatRateBoost, rateCell);
    };
    if(entropyMode==33){
        require(adaptive==0&&enhanced&&extended&&!parent,"Uniform coefficient model requires uniform precision.");
        return selectionOnly?run.template operator()<true,true,true>():run.template operator()<false,true,true>();
    }
    if(entropyMode==32&&adaptive==3&&enhanced&&extended&&!parent)
        return selectionOnly ? run.template operator()<true,true>() : run.template operator()<false,true>();
    return selectionOnly ? run.template operator()<true>() : run.template operator()<false>();
}

template <bool prediction, bool adaptive, uint8_t entropyMode = 23>
static void decodeCurrentSpatialBand(std::span<const uint8_t> coded, float* target, int stride, int width, int height,
                                     float step, bool detail, int parentStride) {
    // The packet reader accepts only this current model. Make those invariants
    // visible to the compiler instead of branching on retired modes per bit.
    constexpr bool enhanced = true, extended = true, median = false;
    constexpr const float* parent = nullptr;
    // The mode-32 context model also serves uniform mode 33. Its public
    // dispatcher validates whether a precision map is present before entry.
    require(entropyMode != 24 || adaptive, "Invalid extended precision stream.");
    SpatialCoderConfig{entropyMode, enhanced, extended, parent != nullptr, parentStride}.validate(width, height, stride,
                                                                                                  step);
    require(target != nullptr, "Missing coefficient output.");
    Decoder coder(coded, enhanced, extended, parent != nullptr, entropyMode);
    Models models(entropyMode);
    const int tileExtent = spatialTileExtent(entropyMode), tileCols = (width + tileExtent - 1) / tileExtent;
    std::vector<int> history(extended ? size_t(width) * 17 : 0);
    Bytes occupiedTiles(extended ? size_t(tileCols) * ((height + tileExtent - 1) / tileExtent) : 0);
    PatternDictionary patterns;
    for (int ty = 0; ty < height; ty += tileExtent)
        for (int tx = 0; tx < width; tx += tileExtent) {
            if (extended && tx == 0)
                for (int y = ty; y < std::min(height, ty + tileExtent); ++y)
                    std::fill_n(history.data() + size_t(y % 17) * width, width, 0);
            const size_t tileAt = size_t(ty / tileExtent) * tileCols + tx / tileExtent;
            const int w = std::min(tileExtent, width - tx), h = std::min(tileExtent, height - ty);
            const int tileContext = extended ? (parent ? 448 : 224) + (tx ? bool(occupiedTiles[tileAt - 1]) : 0) +
                                                   2 * (ty ? bool(occupiedTiles[tileAt - tileCols]) : 0) +
                                                   (parent ? 4 * parentGroup(parent, parentStride, tx, ty, w, h) : 0)
                                             : 0;
            if (!coder.bit(models.p[tileContext]))
                continue;
            if (extended)
                occupiedTiles[tileAt] = 1;
            int neighborhood[18 * 18]{};
            int* known = neighborhood + 19;
            loadHalo(known, history, width, height, tx, ty);
            float localStep = step;
            if (adaptive && coder.bit(models.p[204])) {
                const bool coarse = coder.bit(models.p[205]);
                const bool extra = (entropyMode == 24 || entropyMode == 32) && coder.bit(models.p[206]);
                localStep *= coarse ? (extra ? 4.f : 2.f) : (extra ? .25f : .5f);
            }
            int mode = 0;
            if (prediction && coder.bit(models.p[200])) {
                mode = coder.bit(models.p[201]) ? 2 : 1;
                if (median && mode == 2 && coder.bit(models.p[202]))
                    mode = 3;
            }
            bool tileChild = false;
            for (int sy = 0; sy < h; sy += 8)
                for (int sx = 0; sx < w; sx += 8) {
                    const int ey = std::min(h, sy + 8), ex = std::min(w, sx + 8);
                    const bool inferGroup = entropyMode >= 13 && !prediction && !tileChild && ey == h && ex == w;
                    if (!inferGroup && !coder.bit(models.p[parent ? 432 + groupContext(known, sx, sy, 8) +
                                                                        4 * parentGroup(parent, parentStride, tx + sx,
                                                                                        ty + sy, ex - sx, ey - sy)
                                                           : enhanced ? 216 + groupContext(known, sx, sy, 8)
                                                                      : 1]))
                        continue;
                    tileChild = true;
                    bool groupChild = false;
                    for (int by = sy; by < ey; by += 4)
                        for (int bx = sx; bx < ex; bx += 4) {
                            const int ry = std::min(ey, by + 4), rx = std::min(ex, bx + 4);
                            const bool inferActive = entropyMode >= 13 && !groupChild && ry == ey && rx == ex;
                            if (!inferActive &&
                                !coder.bit(models.p[parent ? 440 + groupContext(known, bx, by, 4) +
                                                                 4 * parentGroup(parent, parentStride, tx + bx, ty + by,
                                                                                 rx - bx, ry - by)
                                                    : enhanced ? 220 + groupContext(known, bx, by, 4)
                                                               : 2]))
                                continue;
                            groupChild = true;
                            bool coefficientChild = false;
                            int reference = -1;
                            bool deltaAny = true;
                            std::array<int, 16> referenceValues{};
                            if ((entropyMode == 17 || entropyMode == 18) && patterns.size && coder.bit(models.p[488])) {
                                reference = 0;
                                for (unsigned b = std::bit_width(patterns.size - 1); b-- > 0;)
                                    reference |= int(coder.raw()) << b;
                                require(unsigned(reference) < patterns.size, "Invalid pattern reference.");
                                const auto& pattern = patterns.entries[reference];
                                require(pattern.width == rx - bx && pattern.height == ry - by,
                                        "Pattern dimensions differ.");
                                referenceValues = pattern.values;
                                deltaAny = coder.bit(models.p[489]);
                            }
                            if (reference < 0 || deltaAny)
                                for (int y = by; y < ry; ++y)
                                    for (int x = bx; x < rx; ++x) {
                                        const bool inferCoefficient =
                                            entropyMode >= 12 && !coefficientChild && y + 1 == ry && x + 1 == rx;
                                        const unsigned extraContext = entropyMode == 15 && !coefficientChild ? 464 : 0;
                                        if (!inferCoefficient &&
                                            !coder.bit(
                                                models.p[extraContext +
                                                         (parent
                                                              ? 400 + 16 * (parent[size_t((ty + y) / 2) * parentStride +
                                                                                   (tx + x) / 2] != 0)
                                                              : 8) +
                                                         context(known, x, y, entropyMode)]))
                                            continue;
                                        coefficientChild = true;
                                        unsigned magnitude = 0;
                                        if (entropyMode == 25 || entropyMode == 26 || entropyMode == 29 ||
                                            entropyMode == 30)
                                            magnitude = decodeBaseMagnitude(coder, models, known, x, y, entropyMode);
                                        else {
                                            unsigned power = 0;
                                            const int c = magnitudeContext(known, x, y);
                                            const bool precise =
                                                entropyMode == 1 || entropyMode == 4 || entropyMode == 5;
                                            while (true) {
                                                const int pc =
                                                    enhanced ? powerContext(known, x, y, precise ? int(power) : -1) : c;
                                                if (!coder.bit(models.p[(enhanced ? 256 : 32) + power * 4 + pc]))
                                                    break;
                                                require(power < unsigned(prediction || (entropyMode == 17 ||
                                                                                        entropyMode == 18)
                                                                             ? 21
                                                                             : 20),
                                                        "Invalid spatial magnitude.");
                                                ++power;
                                            }
                                            magnitude = 1u << power;
                                            for (unsigned b = power; b-- > 0;) {
                                                const unsigned rawBits = entropyMode == 2 || entropyMode == 4   ? 1
                                                                         : entropyMode == 3 || entropyMode == 5 ? 2
                                                                                                                : 0;
                                                if (b < rawBits
                                                        ? coder.raw()
                                                        : coder.bit(
                                                              models.p[enhanced ? 352 + std::min(7u, b) * 4 +
                                                                                      refinementContext(known, x, y, b)
                                                                                : 120 + std::min(7u, b)]))
                                                    magnitude |= 1u << b;
                                            }
                                        }
                                        require(magnitude <= ((prediction || (entropyMode == 17 || entropyMode == 18))
                                                                  ? 2097152u
                                                                  : 1048576u),
                                                "Invalid spatial coefficient.");
                                        const int value =
                                            coder.bit(models.p[128 + signContext(known, x, y, entropyMode)])
                                                ? -int(magnitude)
                                                : int(magnitude);
                                        known[y * contextStride + x] = value;
                                        if (!prediction && entropyMode != 17 && entropyMode != 18)
                                            target[size_t(ty + y) * stride + tx + x] =
                                                (detail ? float(value) + std::copysign(.15f, float(value))
                                                        : float(value)) *
                                                localStep;
                                    }
                            if ((entropyMode == 17 || entropyMode == 18)) {
                                PatternDictionary::Entry original;
                                original.width = rx - bx;
                                original.height = ry - by;
                                for (int y = by; y < ry; ++y)
                                    for (int x = bx; x < rx; ++x) {
                                        const int at = (y - by) * 4 + x - bx;
                                        const int64_t value =
                                            int64_t(reference >= 0 ? referenceValues[at] : 0) +
                                            (reference < 0 || deltaAny ? known[y * contextStride + x] : 0);
                                        require(std::abs(value) <= (prediction ? 2097152 : 1048576),
                                                "Invalid reconstructed pattern coefficient.");
                                        original.values[at] = known[y * contextStride + x] = int(value);
                                        if (!prediction && value)
                                            target[size_t(ty + y) * stride + tx + x] =
                                                (detail ? float(value) + std::copysign(.15f, float(value))
                                                        : float(value)) *
                                                localStep;
                                    }
                                patterns.remember(original);
                            }
                        }
                }
            if (extended)
                for (int y = 0; y < h; ++y)
                    std::copy_n(known + y * contextStride, w, history.data() + size_t((ty + y) % 17) * width + tx);
            if (prediction)
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const int neighbor = predicted(known, x, y, mode, contextStride);
                        const int value = known[y * contextStride + x] + neighbor;
                        require(std::abs(value) <= 1048576, "Invalid predicted spatial coefficient.");
                        known[y * contextStride + x] = value;
                        if (value)
                            target[size_t(ty + y) * stride + tx + x] =
                                (detail ? float(value) + std::copysign(.15f, float(value)) : float(value)) * localStep;
                    }
        }
    coder.input.end();
}
void decodeSpatialBand(std::span<const uint8_t> coded, float* target, int stride, int width, int height, float step,
                       bool detail, bool prediction, bool median, bool adaptive, bool enhanced, bool extended,
                       const float* parent, int parentStride, uint8_t mode) {
    require((mode == 23 || mode == 24 || mode == 31 || mode == 32 || mode == 33) && enhanced && extended && !median && !parent,
            "Unsupported retired spatial decoder.");
    if(mode==33){
        require(!adaptive,"Uniform coefficient model cannot carry adaptive precision.");
        require(!coded.empty()&&coded.front()==4,"Invalid uniform coefficient model selector.");
        if(prediction)decodeCurrentSpatialBand<true,false,32>(coded,target,stride,width,height,step,detail,parentStride);
        else decodeCurrentSpatialBand<false,false,32>(coded,target,stride,width,height,step,detail,parentStride);
        return;
    }
    if (mode == 24 || mode == 32) {
        if (mode == 32) {
            require(adaptive, "Fine local precision requires AQ.");
            if (prediction) decodeCurrentSpatialBand<true,true,32>(coded,target,stride,width,height,step,detail,parentStride);
            else decodeCurrentSpatialBand<false,true,32>(coded,target,stride,width,height,step,detail,parentStride);
            return;
        }
        require(adaptive, "Extended local precision requires adaptive quantization.");
        if (prediction)
            decodeCurrentSpatialBand<true, true, 24>(coded, target, stride, width, height, step, detail, parentStride);
        else
            decodeCurrentSpatialBand<false, true, 24>(coded, target, stride, width, height, step, detail, parentStride);
        return;
    }
    if (mode == 31) {
        require(!coded.empty() && (coded.front() == 4 || coded.front() == 6 || coded.front() == 7), "Invalid coefficient model selector.");
        if(coded.front()==7){
            require(!adaptive,"Extended magnitude model requires uniform precision.");
            if(prediction)decodeCurrentSpatialBand<true,false,26>(coded,target,stride,width,height,step,detail,parentStride);
            else decodeCurrentSpatialBand<false,false,26>(coded,target,stride,width,height,step,detail,parentStride);
            return;
        }
        if (coded.front() == 6) {
            require(!adaptive, "Short magnitude model requires uniform precision.");
            if (prediction)
                decodeCurrentSpatialBand<true, false, 25>(coded, target, stride, width, height, step, detail,
                                                          parentStride);
            else
                decodeCurrentSpatialBand<false, false, 25>(coded, target, stride, width, height, step, detail,
                                                           parentStride);
            return;
        }
    }
    if (prediction) {
        if (adaptive)
            decodeCurrentSpatialBand<true, true>(coded, target, stride, width, height, step, detail, parentStride);
        else
            decodeCurrentSpatialBand<true, false>(coded, target, stride, width, height, step, detail, parentStride);
    } else {
        if (adaptive)
            decodeCurrentSpatialBand<false, true>(coded, target, stride, width, height, step, detail, parentStride);
        else
            decodeCurrentSpatialBand<false, false>(coded, target, stride, width, height, step, detail, parentStride);
    }
}
Bytes compactSpatialBand(std::span<const uint8_t> coded, int width, int height, bool prediction,
                         std::vector<float>* tileCosts, std::span<const float> selectedIntegers) {
    if (coded.empty())
        return {};
    SpatialCoderConfig{23, true, true, false, 0}.validate(width, height, width, 1.f);
    std::vector<float> integers, candidateCosts;
    require(selectedIntegers.empty() || selectedIntegers.size() == size_t(width) * height,
            "Invalid selected coefficient capture.");
    if (selectedIntegers.empty()) {
        integers.assign(size_t(width) * height, 0.f);
        decodeSpatialBand(coded, integers.data(), width, width, height, 1.f, false, prediction, false, false, true,
                          true, nullptr, 0, coded.front()==4?23:31);
        selectedIntegers = integers;
    }
    auto candidate = encodeSpatialBand(selectedIntegers.data(), width, width, height, 1.f, prediction, .5f, false, 0,
                                       true, 0, true, true, .02f, nullptr, 0, nullptr, 0, 25, 0, 0, 0, nullptr, 0,
                                       false, 90, {}, 0, {}, tileCosts ? &candidateCosts : nullptr);
    if (!candidate.empty() && candidate.size() < coded.size()) {
        candidate[0] = 6;
        if (tileCosts)
            *tileCosts = std::move(candidateCosts);
        coded=candidate;
    }
    // Values 0..3 follow exactly the same contexts and decisions in both
    // magnitude models. A second encoding cannot be shorter for that band.
    // Prediction may increase residual magnitudes, so keep its full trial.
    if(!prediction&&std::none_of(selectedIntegers.begin(),selectedIntegers.end(),[](float value){return std::abs(value)>3;}))
        return Bytes(coded.begin(),coded.end());
    auto wider=encodeSpatialBand(selectedIntegers.data(),width,width,height,1.f,prediction,.5f,false,0,
        true,0,true,true,.02f,nullptr,0,nullptr,0,26,0,0,0,nullptr,0,false,90,{},0,{},tileCosts?&candidateCosts:nullptr);
    if(!wider.empty()&&wider.size()<coded.size()){
        wider[0]=7;
        if(tileCosts)*tileCosts=std::move(candidateCosts);
        return wider;
    }
    return Bytes(coded.begin(), coded.end());
}
} // namespace fajk
