#pragma once
#include "fajk/compute.hpp"
#include "color_model_statistics.hpp"
#include <array>
namespace fajk {
struct GpuPlane { std::span<float> pixels; uint32_t width, height; };
// Batched transfer: all planes stay on the device through both axes/all levels.
// On failure, host inputs are untouched and the caller runs the CPU path.
bool gpuWavelet(std::span<GpuPlane>, bool inverse, int firstLevel = 0, int levelCount = 6);
bool gpuResample(const Frame&, uint32_t, uint32_t, std::span<uint8_t>);
struct GpuSourceStatistics {
    uint32_t xx4=0,yy4=0;int32_t xy4=0;
    uint32_t sum[3]{};int32_t sumX2[3]{},sumY2[3]{};uint32_t square[3]{};
    uint32_t quiet=0,strong=0,opaque=1,padding[2]{};
};
bool gpuSourceStatistics(const Frame&,uint32_t cell,int mode,std::span<GpuSourceStatistics>);
struct GpuNativeSettings {
    uint32_t width,height,smoothing,guide,tile;
    bool chroma420,chroma422;
    std::span<const uint8_t> cflMap;
    uint32_t cleanup=0,strength=0,lumaStrength=0,filterPlanes=3;
};
bool gpuDecodeNative(std::span<GpuPlane>,const GpuNativeSettings&,std::span<uint8_t>);
bool gpuSourceColor(const Frame&,std::span<GpuPlane>);
bool gpuSourceColorModel(const Frame&,std::span<GpuPlane>,uint32_t tile,std::span<GpuColorStatistics>,std::span<ColorModelPatch>);
// Reuse the GPU color planes from this immutable encode job, prepare CfL/4:2:0
// and run the forward transform without uploading CPU float planes.
bool gpuSourceWavelet(const Frame&,std::span<GpuPlane>,uint32_t tile,std::span<const uint8_t> map,std::span<const float> means);
// Uses the decoder's cached coordinates and bounded AVX2 edge interpolation.
// Failure leaves output intact. Only packed RGBA crosses the device boundary.
bool gpuEdgeUpscale(const Frame&,uint32_t width,uint32_t height,std::span<uint8_t>,
                    std::span<const int> xLow,std::span<const float> xFraction,
                    std::span<const int> yLow,std::span<const float> yFraction,float sharpness);
// One upload/fence/readback for both operations; the intermediate RGBA stays
// resident. Source/output remain intact if the optional GPU path fails.
bool gpuContourUpscale(const Frame&,uint32_t width,uint32_t height,std::span<uint8_t>,
                      std::span<const int>,std::span<const float>,std::span<const int>,std::span<const float>,
                      float sharpness,float amount,float stronger,float curvature,std::span<const uint8_t> skipTiles,bool fused);
// Immutable RGBA input; only the finished byte image is downloaded. On failure
// output is untouched, allowing the established CPU filter to run.
bool gpuContourCleanup(std::span<uint8_t> output,std::span<const uint8_t> source,
                       uint32_t width,uint32_t height,float amount,float stronger,
                       float curvature,std::span<const uint8_t> skipTiles,bool fused);
bool gpuFinalSharpen(std::span<uint8_t>,uint32_t width,uint32_t height,uint32_t strength);
}
