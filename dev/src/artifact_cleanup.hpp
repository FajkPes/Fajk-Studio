#pragma once
#include "resample.hpp"
#include "contour_grid.hpp"
#include <iosfwd>
namespace fajk {
inline void validateContourTuning(ContourTuning tuning){
    require(tuning.tolerance>=25&&tuning.tolerance<=200&&tuning.minimumSize>=25&&tuning.minimumSize<=200,"Invalid contour tuning.");
}
struct ArtifactGuidePlan {
    uint32_t width=0,height=0,grid=0;float contours=0;
    std::shared_ptr<const void> storage;
    std::span<const float> guides;
    std::vector<float> gpuGuides;
    uint64_t sourceStamp=0;
};
std::shared_ptr<const ArtifactGuidePlan> prepareArtifactGuidePlan(std::span<const uint8_t>,uint32_t,uint32_t,uint32_t,float surfaces,float contours,uint64_t sourceStamp=0,ContourTuning tuning={});
void renderArtifactGuidePlan(std::span<uint8_t>,uint32_t workers,const ArtifactGuidePlan&);
// Frozen overlapping RGB guides drive native contour reconstruction and
// side-aware color cleanup. The default has no separate surface pass.
void artifactCleanupAvx2(std::span<uint8_t>,uint32_t,uint32_t,uint32_t,float,float,uint32_t,ContourTuning tuning={});
// Developer-only guide report; never runs on the production decode path.
void reportContourGuides(const Frame&, uint32_t grid, std::ostream&, bool recoverMissing=true,float contours=2);
Frame contourEffectMap(const Frame&, uint32_t grid, float contours, bool recoverMissing=true,ContourTuning tuning={});
inline void artifactCleanup(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float surfaces,float contours,uint32_t spacing=256,ContourTuning tuning={}){
    validateContourTuning(tuning);
    require(pixels.size()==pixelBytes(width,height)&&std::isfinite(surfaces)&&std::isfinite(contours)&&surfaces>=0&&surfaces<=2&&contours>=0&&contours<=2&&spacing==256,"Invalid unified reconstruction controls.");
    if(hasAvx2()&&(surfaces||contours))artifactCleanupAvx2(pixels,width,height,workers,surfaces,contours,spacing,tuning);
}
}
