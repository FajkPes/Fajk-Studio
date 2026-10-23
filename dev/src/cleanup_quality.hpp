#pragma once
#include "fajk/types.hpp"
#include <cmath>
namespace fajk {
struct CleanupQuality {float surfaces=1,contours=1,antialias=1;};
// Strength follows coded density, not the absolute byte count. This curve is
// assessed on equal packets at low, moderate, and high density.
inline CleanupQuality cleanupQuality(uint32_t bytes,uint32_t width,uint32_t height){
    pixelBytes(width,height);if(!bytes)throw std::runtime_error("Missing coded size for cleanup.");
    const double equivalent=double(bytes)*double(2560*1440)/(double(width)*height);
    const float ratio=float(30000./std::max(30000.,equivalent));
    auto smooth=[](float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);};
    // Base fade retains the approved 30 kB endpoint. A separate envelope
    // below keeps moderate-rate contours active without broad surface blur.
    const float moderate=1-.2f*smooth(float((equivalent-30000.)/50000.));
    // At 150 kB / 1440p the old full pass changed only ~1-2% of pixels,
    // almost entirely by one byte, without a visible benefit on three scenes.
    // Fade to identity smoothly; do not pay for reconstructing intact detail.
    const float taper=smooth(float((150000.-equivalent)/70000.));
    const float aaTaper=smooth(float((100000.-equivalent)/60000.));
    CleanupQuality result{std::pow(ratio,1.8f)*moderate*taper,
                          std::pow(ratio,1.35f)*moderate*taper,
                          std::pow(ratio,1.1f)*aaTaper};
    // Retain moderate-rate contours without adding broad surface smoothing.
    // The final contour floor below preserves the explicit control at high rates.
    result.contours=std::max(result.contours,.9f*std::pow(ratio,1.1f)*taper);
    // The user's contour control must remain effective on detailed packets.
    // Keep broad surface smoothing/AA on their original fade, but retain a
    // bounded contour correction instead of silently disabling it at 150 kB.
    result.contours=std::max(result.contours,.45f+.55f*ratio);
    result.antialias=std::max(result.antialias,.11f*taper);
    // Avoid an expensive pass whose default blend cannot alter an 8-bit level.
    if(result.antialias<1.f/512)result.antialias=0;
    return result;
}
}
