#pragma once
#include <cstdint>
#include <array>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>

namespace fajk {
using Bytes = std::vector<uint8_t>;
using Path = std::filesystem::path;
constexpr uint32_t maxDimension = 16384;
constexpr uint64_t maxPixels = 64ull * 1024 * 1024;
constexpr uint64_t maxPacket = 512ull * 1024 * 1024;
constexpr uint64_t maxFrames = 10000000;
inline size_t pixelBytes(uint32_t w, uint32_t h) {
    if (!w || !h || w > maxDimension || h > maxDimension || uint64_t(w) * h > maxPixels)
        throw std::runtime_error("Unsupported image dimensions (maximum 64 megapixels).");
    return size_t(w) * h * 4;
}
// RGBA8, straight alpha, sRGB. Each frame owns its pixels and timestamp.
struct Frame {
    uint32_t width = 0, height = 0;
    int64_t pts = 0, duration = 0; // 100 ns, same timebase as Media Foundation
    Bytes pixels;
    void validate() const {
        if (pixels.size() != pixelBytes(width, height))
            throw std::runtime_error("Invalid pixel buffer.");
    }
};
enum class CodecId : uint32_t { Fajk = 3 };
enum class EncoderRevision : uint32_t { Experimental011441R441 = 56 };
enum class ComputeBackend : uint32_t { Cpu = 0, Gpu = 1 };
struct BudgetHeatmap {
    struct Tile {
        uint32_t x, y, width, height, channel, level;
        float bits, penalty, quantizer = 1;
    };
    // Coordinates of the encoded working image (possibly downsampled).
    // Display the complete grid over the complete reconstructed source image.
    uint32_t width = 0, height = 0, cell = 16, cols = 0, rows = 0;
    double mappedBytes = 0;
    double detailBytes = 0, unmappedDetailBytes = 0; // Range-coded bands, including padding outside the visible image.
    std::vector<float> bits; // Entropy-coded band bytes apportioned to source footprints.
    std::array<std::vector<float>,3> penalties; // Actual finest-band RDO rate weights; lower buys precision.
    std::array<std::vector<float>,3> localQuantSteps; // Diagnostic: mean actual finest-band step factors; 0 means no finest details.
    std::vector<float> flatCoverage; // Effective flat eligibility used by allocation, 0..1; diagnostic only.
    uint32_t allocationCols = 0, allocationRows = 0;
    std::vector<float> allocationRates; // Actual finest luma RDO group prices before extra flat boost.
    std::array<double,3> channelBits{}; // Mapped detail payload; shared headers/DC are separate.
    std::vector<Tile> tiles; // Opt-in entropy costs before footprint distribution.
};
struct Reconstruction441 {
    uint32_t intensity = 50, strongerTangents = 0, antialias = 50, contrast = 0;
      uint32_t style = 0; // 0: fast Skirt contours; 1: gentler dering contours.
    bool protectText = false;
    bool operator==(const Reconstruction441&) const = default;
};
enum class WaveletStage : uint8_t { Final, Pilot };
enum class EncodeSpeed : uint8_t { MoreQuality, Fast };
struct EncodeOptions {
    ComputeBackend backend = ComputeBackend::Cpu;
    EncodeSpeed speed = EncodeSpeed::MoreQuality;
    WaveletStage waveletStage = WaveletStage::Final;
    Reconstruction441 reconstruction441;
    uint32_t colorModelTile = 64; // Current native predictor; not a historical profile ID.
    std::shared_ptr<BudgetHeatmap>
        budgetHeatmap; // Single-frame diagnostics, never stored in the packet; do not share across concurrent encodes.
    CodecId codec = CodecId::Fajk;
    EncoderRevision revision = EncoderRevision::Experimental011441R441;
    float quality = 85;
    uint32_t shapeIntensity = 200;        // Approved contour reconstruction, independently of surfaces.
    uint32_t contourIntensity = 0;        // Saved contour/surface reconstruction intensity, 0..200%; 0 disables it.
    bool adaptiveAllocationControls = true; // Independent source allocation controls.
    bool adaptiveQuantizationEnabled = false; // Master switch: source prices and local precision selection.
    uint32_t adaptiveAllocationStrength = 100; // Log-rate contrast, 0..500%.
    uint32_t flatAllocation = 50; // Single UI control: 0 conservative, 50 balanced contrast, 100 stronger flat savings / edge precision.
    uint32_t flatSavings = 0;                  // Extra rate penalty in source-verified smooth regions, 0..500%.
    uint32_t flatThreshold = 0; // 0..100: reclaim mostly-flat mixed regions; 0 preserves the approved baseline.
    uint32_t colorEdgePriority = 100;          // Chroma edge protection relative to luma, 0..500%.
    uint8_t adaptiveQuantizationMode =
        0; // Diagnostics: 0 bounded R114, 1 extended, 2 protected textures, 3 flat-aware, 4 chroma priority.
    uint32_t adaptiveQuantization = 100; // Source-priority strength, 0..1000%; 100 preserves profile.
    bool lossless = false;
    bool chroma420 = true;
    bool chroma422 = false; // Exclusive with chroma420; false/false selects 4:4:4.
    uint8_t patternSimilarity = 90; // Minimum normalized coefficient similarity, 0..100.
    uint32_t threads = 0;           // 0: all available logical processors except two
    uint32_t targetBytes = 0;       // Optional packet budget; zero preserves quality-based quantization.
    bool automaticTarget = false;   // Scale the quality-based byte target by pixel count.
};
inline uint32_t qualityTargetBytes(uint32_t width, uint32_t height, float quality) {
    pixelBytes(width, height);
    if (!std::isfinite(quality))
        throw std::runtime_error("Quality must be finite.");
    const double q = std::clamp(double(quality), 1., 100.);
    const double base = q <= 15 ? 20000 + (q - 1) * 40000 / 14 : 60000 + (q - 15) * 140000 / 85;
    // The automatic packet budget must fit the native headers and reconstruction
    // wrappers even on tiny inputs. Explicit smaller budgets remain validated.
    return uint32_t(std::clamp(base * double(width) * height / (2560. * 1440.), 1024., double(maxPacket)));
}
struct SequenceInfo {
    uint32_t width = 0, height = 0;
    uint32_t fpsNum = 30, fpsDen = 1;
    CodecId codec = CodecId::Fajk;
    uint32_t sourceStep = 1;
    int64_t duration = 0;
    uint64_t count = 0;
    bool temporal = false; // Version 2: direct keyframe references, no dependency chains.
};
struct Packet {
    Bytes data;
    int64_t pts = 0, duration = 0;
    uint64_t referenceIndex = UINT64_MAX;
    std::shared_ptr<const Packet> reference; // Resolved by SequenceReader; never serialized inside the payload.
    std::shared_ptr<const Packet> keyIdentity; // Reader-owned immutable identity for reusing an already decoded key.
};
inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace fajk
