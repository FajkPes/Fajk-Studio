#pragma once
#include <cstdint>

namespace fajk {
// Production reconstruction and diagnostics must use the same guide spacing.
// Moderate corrections tolerate fewer overlapping models. Strong low-rate
// contours retain the dense grid selected in visual review.
inline uint32_t contourGuideSpacing(float surfaces, float contours) {
    const float threshold = surfaces <= .1f ? .8f : .45f;
    return surfaces < .2f && contours < threshold ? 16u : 8u;
}
} // namespace fajk
