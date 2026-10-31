#pragma once
#include "fajk/compute.hpp"
#include "color_model_statistics.hpp"
#include <array>
#include <functional>
namespace fajk {
struct ArtifactGuidePlan;
bool gpuArtifactSupported();
// Optional soft-cache ticket. Eviction is safe: consumers upload the unchanged
// host image on a miss. A ticket never exposes or borrows an engine buffer.
struct GpuDecodedSource {uint64_t stamp=0;const uint8_t* pixels=nullptr;size_t bytes=0;};
class ScopedGpuDecodedSource {
    GpuDecodedSource* previous_;
public:
    explicit ScopedGpuDecodedSource(GpuDecodedSource*);
    ~ScopedGpuDecodedSource();
    ScopedGpuDecodedSource(const ScopedGpuDecodedSource&)=delete;
    ScopedGpuDecodedSource& operator=(const ScopedGpuDecodedSource&)=delete;
};
struct GpuPlane { std::span<float> pixels; uint32_t width, height; };
// Batched transfer: all planes stay on the device through both axes/all levels.
// On failure, host inputs are untouched and the caller runs the CPU path.
bool gpuWavelet(std::span<GpuPlane>, bool inverse, int firstLevel = 0, int levelCount = 6);
// Matches the current CPU inverse for quantized residuals, including scalar normalization tails.
// Optional double-precision support is used only for those tail samples.
bool gpuCanonicalInverse(std::span<GpuPlane>,int firstLevel=0,int levelCount=6);
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
    uint32_t cleanup=0,strength=0,lumaStrength=0,filterPlanes=3,chromaCleanup=0,chromaCleanupStrength=100;
};
// allowSparse=false retains the dense reference route for exact diagnostics.
bool gpuDecodeNative(std::span<GpuPlane>,const GpuNativeSettings&,std::span<uint8_t>,bool allowSparse=true);
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
                      float sharpness,float amount,float stronger,float curvature,std::span<const uint8_t> skipTiles,bool fused,uint32_t sharpening=0,const ArtifactGuidePlan* artifact=nullptr);
// Immutable RGBA input; only the finished byte image is downloaded. On failure
// output is untouched, allowing the established CPU filter to run.
bool gpuContourCleanup(std::span<uint8_t> output,std::span<const uint8_t> source,
                       uint32_t width,uint32_t height,float amount,float stronger,
                       float curvature,std::span<const uint8_t> skipTiles,bool fused);
bool gpuFinalSharpen(std::span<uint8_t>,uint32_t width,uint32_t height,uint32_t strength);
// Exact zero-extra-surfaces base renderer. Guides are frozen CPU 24-float
// records. Output is committed only after all GPU work succeeds.
bool gpuArtifactCleanup(std::span<uint8_t>,uint32_t width,uint32_t height,uint32_t grid,std::span<const float> guides,float contours);
// Canonical integer Max-reference filter; exact CPU pixels, atomic fallback.
bool gpuReferenceFilter(Frame&,uint32_t step,std::span<const uint8_t> keep,bool adjacent=false);
// Integer quarter-pixel camera/object prediction. Packed vectors contain
// signed int16 x/y; optional 8x8 modes use bit 1 for a stationary predictor.
// The result is opaque RGBA. Failed GPU work leaves host output untouched.
bool gpuMotionWarp(const Frame&,uint32_t cell,std::span<const uint32_t> vectors,
                   const std::array<int,3>& bias,std::span<const uint8_t> modes,std::span<uint8_t> output,bool independentBlocks=false,std::span<const uint32_t> affine={},const std::function<void()>& whilePending={});
// whilePending is optional independent CPU work executed after submission,
// while the engine lock protects in-flight resources. It must not call GPU
// APIs or access output. A declined/failed GPU operation may not have called
// it; callers track completion before fallback. CPU exceptions propagate.
// Resident prediction -> canonical residual color -> optional reference filter.
// The callback produces inverse planes while prediction runs on the device.
// Only final RGBA returns; failed GPU work leaves the output untouched.
bool gpuReconstructMotion(const Frame&,std::span<const uint32_t> vectors,
    const std::array<int,3>& bias,std::span<const uint8_t> modes,std::span<const uint32_t> affine,
    std::span<GpuPlane> inversePlanes,uint32_t filterStep,std::span<uint8_t> output,
    const std::function<void()>& whilePending={},bool coefficients=false,bool adjacentFilter=false);
// With coefficients=true the planes are quantized transform coefficients:
// canonical inverse, color and filtering all stay on-device. Sparse inputs
// upload exact index/float-bit pairs instead of dense zero planes. The CPU
// callback is not called on supported hardware. Without double precision the provided
// callback supplies the CPU inverse, or the operation declines if it is absent.
// Two GPU passes: coarse 128-pixel seeds, then independent 32-pixel quarter-pel
// blocks. Only packed signed int16 x/y vectors return to the CPU; the coarse
// field stays on the device. No encoder-format or quality decisions are made.
bool gpuBlockMotion(const Frame& current,const Frame& reference,std::span<uint32_t> vectors,std::span<uint32_t> affine={});
// Prepare signed YCoCg/4:2:0 residuals and run all forward levels on-device.
// No float-plane upload or intermediate readback. Inputs are immutable and a
// failure leaves host coefficient planes untouched for the CPU fallback.
bool gpuPredictiveWavelet(const Frame& current,const Frame& predicted,const Frame& witness,
                          std::span<const uint8_t> keep,uint32_t propagatedError,std::span<GpuPlane> planes,bool lumaErrorOnly=false);
// Closed-loop Max preparation: both warps, exact 8x8 mode decisions and forward
// residual transform remain resident. Only selected prediction/modes and final
// coefficients return, with one fence. Failure leaves every output untouched.
bool gpuPreparePredictive(const Frame& current,const Frame& sourceReference,const Frame& decodedReference,
                          std::span<const uint32_t> vectors,std::span<const uint32_t> affine,
                          const std::array<int,3>& bias,uint32_t tolerance,uint32_t propagatedError,
                          std::span<uint8_t> predicted,std::span<uint8_t> keep,std::span<GpuPlane> planes,bool lumaErrorOnly=false);
}
