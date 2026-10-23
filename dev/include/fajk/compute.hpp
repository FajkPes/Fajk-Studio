#pragma once
#include "types.hpp"
#include <string>
namespace fajk {
// Execution choice only. Both backends read and write the R470 packet family.
struct GpuInfo { bool available = false; std::string device, error; };
GpuInfo gpuInfo();
// Scope is local to the calling codec job; it never changes another decoder.
class ScopedComputeBackend {
    ComputeBackend previous_;
    uint64_t previousStamp_;
public:
    explicit ScopedComputeBackend(ComputeBackend);
    ~ScopedComputeBackend();
    ScopedComputeBackend(const ScopedComputeBackend&) = delete;
    ScopedComputeBackend& operator=(const ScopedComputeBackend&) = delete;
};
ComputeBackend computeBackend();
struct GpuActivity { uint64_t transforms = 0, resamples = 0, uploadedBytes = 0, downloadedBytes = 0, fallbacks = 0, analyses = 0, nativeDecodes = 0, contourFilters = 0, sharpens = 0; };
GpuActivity gpuActivity();
}
