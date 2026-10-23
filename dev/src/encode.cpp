#include "codec_internal.hpp"
#include "decode_internal.hpp"
#include "wavelet.hpp"
#include "color_prediction.hpp"
#include "flat_regions.hpp"
#include "resample.hpp"
#include "reconstruction441.hpp"
#include "fajk/hardware.hpp"
#include <cstring>
namespace fajk {
namespace {
std::optional<Bytes> paletteFrame(const Frame& f, const EncodeOptions& o, bool raw, bool uniformOnly = false) {
    // Palettes must preserve the source colors. Quantizing before counting colors
    // made photographic inputs select this mode at LOW quality and grow in size.
    uint32_t solid;
    std::memcpy(&solid, f.pixels.data(), 4);
    bool uniform = true;
    const auto expected = _mm_set1_epi32(int(solid));
    size_t at = 0;
    for (; at + 16 <= f.pixels.size(); at += 16)
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(f.pixels.data() + at)),
                                              expected)) != 65535) {
            uniform = false;
            break;
        }
    if (uniform)
        for (; at < f.pixels.size(); at += 4) {
            uint32_t value;
            std::memcpy(&value, f.pixels.data() + at, 4);
            if (value != solid) {
                uniform = false;
                break;
            }
        }
    if (uniform && !raw) {
        ByteWriter out;
        out.u32(frameMagic);
        out.u8(4);
        out.u8(2);
        out.u8(uint8_t(o.quality));
        out.u8(0);
        out.u32(f.width);
        out.u32(f.height);
        out.u16(1);
        out.u32(solid);
        out.u32(6);
        out.u8(4);
        out.u32(uint32_t(f.pixels.size() / 4));
        out.u8(0);
        return std::move(out.data);
    }
    if(uniformOnly)
        return std::nullopt;
    std::array<uint32_t, 8192> colors;
    std::array<uint16_t, 8192> lookup;
    lookup.fill(UINT16_MAX);
    std::vector<uint32_t> palette;
    palette.reserve(4096);
    std::vector<uint16_t> indices;
    indices.reserve(std::min<size_t>(16384, size_t(f.width) * f.height));
    for (size_t i = 0; i < f.pixels.size();) {
        uint32_t color;
        std::memcpy(&color, f.pixels.data() + i, 4);
        size_t slot = (color * 2654435761u) >> 19;
        while (lookup[slot] != UINT16_MAX && colors[slot] != color)
            slot = (slot + 1) & 8191;
        if (lookup[slot] == UINT16_MAX) {
            if (palette.size() == 4096)
                return std::nullopt;
            lookup[slot] = uint16_t(palette.size());
            colors[slot] = color;
            palette.push_back(color);
        }
        size_t run = 1;
        const auto repeated = _mm_set1_epi32(int(color));
        while (i + (run + 4) * 4 <= f.pixels.size() &&
               _mm_movemask_epi8(_mm_cmpeq_epi32(
                   _mm_loadu_si128(reinterpret_cast<const __m128i*>(f.pixels.data() + i + run * 4)), repeated)) ==
                   65535)
            run += 4;
        if (indices.size() + run >= 16384 && indices.capacity() < f.pixels.size() / 4)
            indices.reserve(f.pixels.size() / 4);
        indices.insert(indices.end(), run, lookup[slot]);
        i += run * 4;
    }
    Bytes low(indices.size()), high(palette.size() > 256 ? indices.size() : 0);
    for (size_t i = 0; i < indices.size(); ++i) {
        uint16_t predictor = i >= f.width ? indices[i - f.width] : 0;
        uint16_t residual = uint16_t(indices[i] - predictor);
        low[i] = uint8_t(residual);
        if (!high.empty())
            high[i] = uint8_t(residual >> 8);
    }
    ByteWriter out;
    out.u32(frameMagic);
    out.u8(4);
    out.u8(2);
    out.u8(uint8_t(o.quality));
    out.u8(0);
    out.u32(f.width);
    out.u32(f.height);
    out.u16(uint16_t(palette.size()));
    for (auto color : palette)
        out.u32(color);
    appendStream(out, low, raw);
    if (!high.empty())
        appendStream(out, high, raw);
    return std::move(out.data);
}

uint32_t targetFor(const Frame& f, const EncodeOptions& o) {
    return o.targetBytes ? o.targetBytes : o.automaticTarget ? qualityTargetBytes(f.width, f.height, o.quality) : 0;
}
Packet encodeLossless(const Frame& f, const EncodeOptions& o) {
    if (auto palette = paletteFrame(f, o, false)) {
        require(!o.targetBytes || palette->size() <= o.targetBytes, "Lossless palette exceeds target.");
        return {std::move(*palette), f.pts, f.duration};
    }
    ByteWriter out;
    out.u32(frameMagic);
    out.u8(4);
    out.u8(1);
    out.u8(uint8_t(o.quality));
    out.u8(0);
    out.u32(f.width);
    out.u32(f.height);
    for (int c = 0; c < 4; ++c)
        appendStream(out, predictive(f, c));
    require(out.data.size() <= maxPacket && (!o.targetBytes || out.data.size() <= o.targetBytes),
            "Lossless frame exceeds target.");
    return {std::move(out.data), f.pts, f.duration};
}
Packet encodeNativeContent(const Frame& f, EncodeOptions o, bool opaque) {
    const uint32_t target = targetFor(f, o);
    o.targetBytes = target;
    o.automaticTarget = false;
    const bool low = target && uint64_t(target) * 1000 < uint64_t(f.width) * f.height * 35;
    // R441's selected color model: fine cells at high density, normal cells at low density.
    o.colorModelTile = low ? 64 : 32;
    if (!low || target < 4096 || f.width < 128 || f.height < 128 || !opaque)
        return encodeColorPrediction(f, o, opaque);
    // At the lowest coded density, a modest reduction leaves more coefficients
    // for each boundary and reduces ringing. Return smoothly to the original
    // working resolution as density rises; the byte count alone is insufficient.
    const double equivalent=double(target)*double(2560*1440)/(double(f.width)*f.height);
    const double t=std::clamp((equivalent-30000.)/10000.,0.,1.);
    const double workingScale=(12+t*t*(3-2*t))/16.;
    auto small = resamplePhoto(f, uint32_t(f.width*workingScale+.5), uint32_t(f.height*workingScale+.5), o.threads, {}, 2);
    auto childOptions = o;
    childOptions.targetBytes = target - 28;
    childOptions.colorModelTile = 64;
    auto child = encodeColorPrediction(small, childOptions, true);
    // Coarse chroma cleanup remains useful at moderate density. At the lowest
    // rates the shared RGB reconstruction preserves contours better without it.
    if (uint64_t(target) * 1000 >= uint64_t(f.width) * f.height * 15 && child.data.size() > 39 && child.data[4] == 55) {
        ByteReader inner{child.data};
        inner.bytes(7);
        inner.bytes(inner.var());
        const auto at = inner.pos;
        if (child.data.size() > at + 31 && child.data[at + 4] == 114 && child.data[at + 24] == 8)
            child.data[at + 25] |= uint8_t(11 << 2);
    }
    ByteWriter out;
    out.u32(frameMagic);
    out.u8(28);
    out.u8(uint64_t(f.width) * f.height <= 4 * 1024 * 1024 ? 55 : 14);
    out.u16(0);
    out.u32(f.width);
    out.u32(f.height);
    out.u32(small.width);
    out.u32(small.height);
    out.u32(uint32_t(child.data.size()));
    out.bytes(child.data);
    require(out.data.size() <= target, "Native content exceeds target.");
    return {std::move(out.data), f.pts, f.duration};
}
Packet encodeFlatContent(const Frame& f, EncodeOptions o, bool opaque) {
    const auto target = targetFor(f, o);
    if (target < 4096 || !opaque)
        return encodeNativeContent(f, o, opaque);
    const bool fine = uint64_t(target) * 1000 < uint64_t(f.width) * f.height * 35;
    const auto metadata = flatRegions(f, fine ? 8 : 16, o.threads);
    if (metadata.empty())
        return encodeNativeContent(f, o, opaque);
    auto coded = entropyEncode(metadata, true);
    bool predicted = false;
    auto alternate = entropyEncode(predictFlatRegions(metadata, f.width, f.height), true);
    if (alternate.size() < coded.size()) {
        coded = std::move(alternate);
        predicted = true;
    }
    const uint32_t overhead = 24 + uint32_t(coded.size());
    if (uint64_t(overhead) * 100 > uint64_t(target) * 16 || target <= overhead + 4096)
        return encodeNativeContent(f, o, opaque);
    o.targetBytes = target - overhead;
    o.automaticTarget = false;
    const auto child = encodeNativeContent(f, o, opaque);
    ByteWriter out;
    out.u32(frameMagic);
    out.u8(77);
    out.u8(predicted ? 2 : 1);
    out.u16(0);
    out.u32(f.width);
    out.u32(f.height);
    out.u32(uint32_t(coded.size()));
    out.u32(uint32_t(child.data.size()));
    out.bytes(coded);
    out.bytes(child.data);
    require(out.data.size() <= target, "Flat content exceeds target.");
    return {std::move(out.data), f.pts, f.duration};
}
Packet encodeBase(const Frame& f, EncodeOptions o, bool opaque) {
    const auto target = targetFor(f, o);
    if (o.lossless)
        return encodeLossless(f, o);
    if (!opaque)
        return encodeNativeContent(f, o, opaque);
    constexpr uint32_t overhead = 36;
    require(!target || target >= overhead + 256, "Reconstruction target is too small.");
    o.targetBytes = target ? target - overhead : 0;
    o.automaticTarget = false;
    auto child = encodeFlatContent(f, o, opaque);
    const auto budget = uint32_t((target ? target : child.data.size() + overhead) + 168);
    size_t offset = 0;
    const bool flat = child.data.size() >= 24 && child.data[4] == 77;
    if (flat) {
        ByteReader header{child.data};
        header.bytes(16);
        const auto meta = header.u32(), bytes = header.u32();
        require(uint64_t(meta) + bytes + 24 == child.data.size(), "Invalid flat content.");
        offset = 24 + meta;
    }
    bool workingFiltered = false;
    if (child.data.size() >= offset + 28 && child.data[offset + 4] == 28) {
        child.data[offset + 5] = 130;
        ByteWriter controls;
        controls.u32(budget);
        controls.u16(uint16_t(o.contourIntensity));
        controls.u16(uint16_t(o.shapeIntensity));
        child.data.insert(child.data.end(), controls.data.begin(), controls.data.end());
        if (flat) {
            const auto bytes = uint32_t(child.data.size() - offset);
            for (int i = 0; i < 4; ++i)
                child.data[20 + i] = uint8_t(bytes >> (8 * i));
        }
        workingFiltered = true;
    }
    ByteWriter out;
    out.u32(frameMagic);
    out.u8(129);
    out.u8(12);
    out.u16(uint16_t(o.contourIntensity));
    out.u32(f.width);
    out.u32(f.height);
    out.u32(budget);
    out.u16(uint16_t(o.shapeIntensity));
    out.u16(workingFiltered ? 1 : 0);
    out.u32(uint32_t(child.data.size()));
    out.bytes(child.data);
    require(out.data.size() <= maxPacket && (!target || out.data.size() <= target), "Reconstruction exceeds target.");
    return {std::move(out.data), f.pts, f.duration};
}
} // namespace
Packet encodeFrame(const Frame& f, const EncodeOptions& o) {
    require(o.speed==EncodeSpeed::MoreQuality||o.speed==EncodeSpeed::Fast,"Invalid encode speed mode.");
    ScopedComputeBackend compute(o.backend);
    f.validate();
    validate441(o.reconstruction441);
    require(o.revision == EncoderRevision::Experimental011441R441, "Only R441 is supported.");
    require(std::isfinite(o.quality) && o.quality >= 1 && o.quality <= 100, "Quality must be 1-100.");
    require(o.contourIntensity <= 200 && o.shapeIntensity <= 200, "Reconstruction intensity must be 0-200.");
    require(o.adaptiveAllocationStrength <= 500 && o.flatSavings <= 500 && o.colorEdgePriority <= 500 &&
                o.adaptiveQuantization <= 1000 && o.flatAllocation <= 100 && o.flatThreshold <= 100,
            "Invalid allocation controls.");
    // Palette/lossless routes have no AQ capture. Do not return maps left over
    // from an earlier wavelet encode when the caller reuses the options object.
    if (o.budgetHeatmap)
        *o.budgetHeatmap = BudgetHeatmap{};
    require(o.targetBytes<=maxPacket,"Invalid packet target.");
    // No transform, color model, AQ or reconstruction can improve an exactly
    // constant RGBA frame. On ordinary images this scan exits at the first
    // differing vector; do not run the general palette search on photographs.
    if(!o.lossless){
        if(auto solid=paletteFrame(f,o,false,true);solid&&(!targetFor(f,o)||solid->size()<=targetFor(f,o)))
            return {std::move(*solid),f.pts,f.duration};
    }
    auto base = o;
    base.threads = safeWorkers(f.width, f.height, o.lossless, o.threads);
    ScopedWorkerBudget workerBudget(base.threads);
    // These layers all inspect the same immutable source. Scan its alpha once,
    // rather than rereading the full RGBA frame in every wrapper.
    const bool opaque = !o.lossless && opaquePhoto(f.pixels);
    if (o.lossless || !opaque)
        return encodeBase(f, base, opaque);
    require(hasAvx2(), "R441 reconstruction requires AVX2.");
    const auto target = targetFor(f, o);
    require(!target || (target >= reconstruction441HeaderBytes + 512 && target <= maxPacket), "Invalid R441 target.");
    base.automaticTarget = false;
    base.targetBytes = target ? target - reconstruction441HeaderBytes : 0;
    const auto child = encodeBase(f, base, opaque);
    // The decoder's quality fade follows the data actually stored, not an
    // unattained requested limit (important for simple, highly compressible inputs).
    const uint32_t budget = uint32_t(child.data.size() + reconstruction441HeaderBytes + 168);
    ByteWriter out;
    out.u32(frameMagic);
    out.u8(reconstruction441Version);
    out.u8(1);
    out.u16(0);
    out.u32(f.width);
    out.u32(f.height);
    out.u32(budget);
    const auto& s = o.reconstruction441;
    out.u16(uint16_t(s.intensity));
    out.u16(uint16_t(s.strongerTangents));
    out.u16(uint16_t(s.antialias));
    out.u16(uint16_t(s.contrast));
    out.u16(uint16_t((s.protectText ? 1 : 0) | (s.style << 1)));
    out.u16(0);
    out.u32(uint32_t(child.data.size()));
    out.bytes(child.data);
    require(!target || out.data.size() <= target, "R441 exceeds target.");
    return {std::move(out.data), f.pts, f.duration};
}
} // namespace fajk
