#pragma once
#include "fajk/types.hpp"
namespace fajk {
inline float flatMinimumCoverage(uint32_t threshold) {
    require(threshold<=100,"Invalid minimum-data threshold.");
    return .95f - .007f * threshold;
}
// Extra savings act after the bounded source classifier. Its 128x endpoint
// must not turn the new control into a no-op. Source-protected prices <=1
// retain their precision; the full boost applies only in flat interiors.
inline float flatRateMultiplier(float sourceRate, float boost) {
    return sourceRate <= 1 || boost == 1 ? 1.f
        : 1.f + (boost - 1.f) * std::min(1.f, (sourceRate - 1.f) / 15.f);
}
// The slider changes the eligibility of mixed footprints: at 100 even a 25%
// smooth footprint is eligible; at 0 it needs >75%. Confirmed non-flat (0)
// stays protected and confirmed smooth (1) stays eligible. 50 is identity.
inline float allocationFlatCoverage(float coverage, uint32_t control) {
    require(std::isfinite(coverage) && coverage >= 0 && coverage <= 1 && control <= 100,
            "Invalid flat allocation coverage.");
    if (control == 50)
        return coverage;
    const float lower = control < 50 ? (50 - control) * .015f : 0.f;
    const float upper = control > 50 ? 1.f - (control - 50) * .015f : 1.f;
    return std::clamp((coverage - lower) / (upper - lower), 0.f, 1.f);
}
// Encoder-only allocation decisions. Local precision flags share the existing
// coefficient stream; the diagnostic maps themselves are never stored.
// A smaller rate penalty buys precision. Keep source-protected edges separate
// from savings in verified smooth regions, and scale their log-rate contrast.
inline float adaptiveAllocationRate(float original, float flatCoverage, bool chroma, const EncodeOptions& options) {
    require(std::isfinite(original) && original >= .25f && original <= 128.f && std::isfinite(flatCoverage) &&
                flatCoverage >= 0 && flatCoverage <= 1,
            "Invalid allocation source weight.");
    float rate = original;
    if (original < 1) {
        const float exponent =
            float(options.adaptiveQuantization) / 100.f * (chroma ? float(options.colorEdgePriority) / 100.f : 1.f);
        if (exponent != 1)
            rate = std::pow(original, exponent);
    } else if (options.flatSavings && flatCoverage > 0) {
        rate *= 1 + float(options.flatSavings) / 100.f * flatCoverage;
    }
    if (options.adaptiveAllocationStrength != 100)
        rate = std::pow(rate, float(options.adaptiveAllocationStrength) / 100.f);
    // Apply to the smooth share even when the footprint also touches an edge
    // halo. The old savings branch skipped all such footprints. A wholly
    // non-flat footprint is unchanged; coarse mean/slope passes coverage=0.
    // Tolerance decides eligibility, not the entire tile's savings. Applying
    // its saturated map to the rate also made a 25%-flat mixed tile pay the
    // same 64x penalty as a wholly smooth tile at 100, destroying weak detail.
    // Concentrate the added saving inside verified flat interiors, not mixed
    // footprints. Direct source geometry has coverage=0 at the call site.
    // Coarse mean/slope bands remain untouched. This is encoder RDO only.
    if (flatCoverage > 0 && options.adaptiveAllocationStrength) {
        const float toleranceShare = options.flatAllocation > 50 ? flatCoverage
            : allocationFlatCoverage(flatCoverage, options.flatAllocation);
        // At 50 the interior price is 16x; 100 already has 64x from the
        // tolerance term. Adding another 256x at 100 only saturated the
        // quantizer and damaged color shading instead of buying cleaner edges.
        const float interior = float(std::min(options.flatAllocation, 100u - options.flatAllocation)) * .08f;
        const float contrast = (float(options.flatAllocation) - 50.f) * .12f * toleranceShare +
                               interior * flatCoverage * flatCoverage;
        rate *= std::exp2(contrast * (float(options.adaptiveAllocationStrength) / 100.f));
    }
    return std::clamp(rate, .03125f, 128.f);
}
} // namespace fajk
