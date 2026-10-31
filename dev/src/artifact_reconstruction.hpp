#pragma once
#include "artifact_cleanup.hpp"
namespace fajk::artifactReconstruction {
// Reference density matches the approved 2560x1440 / 30 kB appearance.
// Uniform image/budget scaling preserves the reconstruction strength.
inline float rateStrength(uint32_t bytes,uint32_t width,uint32_t height){
    const double reference=double(bytes)*double(2560*1440)/(double(width)*height);
    return std::clamp(float(std::pow(30000./std::max(reference,30000.),1.57)),.04f,1.f);
}
inline bool lowRate(uint32_t bytes,uint32_t width,uint32_t height){
    return uint64_t(bytes)*2560*1440<=uint64_t(width)*height*80000;
}
inline float contourStrength(float rate,double equivalent,uint32_t contours){
    // Preserve the approved low-rate contours. At higher density, broad color
    // models must yield to the detail already present in the coded image.
    const float t=std::clamp(float((equivalent-40000.)/110000.),0.f,1.f);
    const float taper=1-t*t*(3-2*t);
    float contourAmount=std::sqrt(rate)*taper*contours/100.f;
    // Below this bound the correction cannot change a rounded 8-bit sample.
    // Near a target of 150 kB, tiny nonzero fades otherwise cost a whole pass.
    if(contourAmount<1.f/512)contourAmount=0;
    return contourAmount;
}
inline std::shared_ptr<const ArtifactGuidePlan> prepareGpu(std::span<const uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,uint32_t bytes,uint32_t surfaces,uint32_t contours,uint32_t sourceWidth=0,uint32_t sourceHeight=0,uint64_t sourceStamp=0,ContourTuning tuning={}){
    if(surfaces||!contours||uint64_t(width)*height<512*512)return {};
    const auto rw=sourceWidth?sourceWidth:width,rh=sourceHeight?sourceHeight:height;
    const auto rate=rateStrength(bytes,rw,rh);
    const double equivalent=double(bytes)*double(2560*1440)/(double(rw)*rh);
    return prepareArtifactGuidePlan(pixels,width,height,workers,0,contourStrength(rate,equivalent,contours),sourceStamp,tuning);
}
inline void restore(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,uint32_t bytes,uint32_t surfaces,uint32_t contours,uint32_t sourceWidth=0,uint32_t sourceHeight=0,ContourTuning tuning={}){
    const float rate=rateStrength(bytes,sourceWidth?sourceWidth:width,sourceHeight?sourceHeight:height);
    const double equivalent=double(bytes)*double(2560*1440)/(double(sourceWidth?sourceWidth:width)*(sourceHeight?sourceHeight:height));
    const float contourAmount=contourStrength(rate,equivalent,contours);
    artifactCleanup(pixels,width,height,workers,rate*surfaces/100.f,contourAmount,256,tuning);
}
}
