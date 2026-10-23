#pragma once
#include "fajk/types.hpp"
#include <array>
#include <chrono>
namespace fajk {
// Opt-in diagnostic worker time, not caller wall time. Each band has its own
// capture; callers merge only after joining workers. No atomics in hot loops.
struct SpatialWorkProfile { std::array<double,5> ms{}; };
inline thread_local SpatialWorkProfile* spatialWorkProfile = nullptr;
struct SpatialWorkCapture {
    SpatialWorkProfile* previous;
    explicit SpatialWorkCapture(SpatialWorkProfile* value):previous(spatialWorkProfile){spatialWorkProfile=value;}
    ~SpatialWorkCapture(){spatialWorkProfile=previous;}
};
struct SpatialTileTimer {
    using Clock=std::chrono::steady_clock;
    SpatialWorkProfile* target=spatialWorkProfile;
    Clock::time_point start=target?Clock::now():Clock::time_point{};
    unsigned part=0;
    void mark(unsigned next){if(target){auto now=Clock::now();target->ms[part]+=std::chrono::duration<double,std::milli>(now-start).count();start=now;part=next;}}
    ~SpatialTileTimer(){mark(part);}
};
inline int spatialTileExtent(uint8_t mode) {
    return mode == 28 || mode == 30 ? 4 : mode == 27 || mode == 29 || mode == 32 ? 8 : 16;
}
inline unsigned spatialAdaptation(uint8_t mode) {
    return mode == 8 || mode == 10 ? 4 : mode == 9 || mode == 11 || mode == 14 || mode == 15 ? 6 : 5;
}
struct SpatialCoderConfig {
    uint8_t entropyMode = 0;
    bool enhanced = false, extended = false, parent = false;
    int parentStride = 0;
    void validate(int width, int height, int stride, float step) const {
        require(width > 0 && height > 0 && width <= int(maxDimension) && height <= int(maxDimension) &&
                    stride >= width && std::isfinite(step) && step > 0,
                "Invalid coefficient band geometry or step.");
        require((entropyMode <= 18 || (entropyMode >= 23 && entropyMode <= 30) || entropyMode == 32) && (entropyMode < 12 || !parent) &&
                    (!entropyMode || extended) && (!extended || enhanced) &&
                    (!parent || (extended && parentStride >= (width + 1) / 2)),
                "Invalid spatial coder configuration.");
    }
};
struct SpatialTileBound {
    float vectorMaximum = 0, tailMaximum = 0;
};
std::vector<SpatialTileBound> spatialTileBounds(const float*, int, int, int, uint8_t);
// Quantized coefficient neighborhoods (16x16, or private 8x8/4x4); the decoder requires an initially
// zeroed destination. Stripes have independent adaptive probability models.
// selectionOnly captures reconstructed base-step units (including local fractions): train the same
// models and return selected integers without producing an entropy stream.
Bytes encodeSpatialBand(const float* source, int stride, int width, int height, float step, bool prediction = false,
                        float rounding = .35f, bool median = false, float rdo = 0, bool omitEmpty = false,
                        uint8_t adaptive = 0, bool enhanced = false, bool extended = false, float adaptiveRate = .02f,
                        const uint8_t* parent = nullptr, int parentStride = 0, uint8_t* significance = nullptr,
                        int significanceStride = 0, uint8_t entropyMode = 0, float quantRdo = 0,
                        float quantGradient = 0, float synthesisCoupling = 0, const float* correlations = nullptr,
                        uint8_t coupledQuantizer = 0, bool rateQuantizer = false, uint8_t patternSimilarity = 90,
                        std::span<const float> tileRates = {}, uint8_t restrictedPrecision = 0,
                        std::span<const SpatialTileBound> bounds = {}, std::vector<float>* tileCosts = nullptr,
                        std::vector<float>* selectedIntegers = nullptr, bool selectionOnly = false,
                        std::span<const float> groupRates = {}, std::vector<float>* tilePrecisions = nullptr, float flatRateBoost = 1.f, uint32_t rateCell = 4);
void decodeSpatialBand(std::span<const uint8_t> coded, float* target, int stride, int width, int height, float step,
                       bool detail, bool prediction = false, bool median = false, bool adaptive = false,
                       bool enhanced = false, bool extended = false, const float* parent = nullptr,
                       int parentStride = 0, uint8_t entropyMode = 0);
// Uniform-precision mode-23/31 band -> shorter lossless representation. The
// returned prefix is 4 (log magnitudes), 6 (three base levels), or 7 (seven
// base levels); decode with mode 31. No source coefficient is requantized.
Bytes compactSpatialBand(std::span<const uint8_t>, int width, int height, bool prediction,
                         std::vector<float>* tileCosts = nullptr,std::span<const float> selectedIntegers = {});
} // namespace fajk
