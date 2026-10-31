#pragma once
#include "fajk/compute.hpp"
namespace fajk {
inline void accumulateGpu(GpuActivity& total,const GpuActivity& after,const GpuActivity& before={}){
    total.transforms+=after.transforms-before.transforms;total.resamples+=after.resamples-before.resamples;
    total.uploadedBytes+=after.uploadedBytes-before.uploadedBytes;total.downloadedBytes+=after.downloadedBytes-before.downloadedBytes;
    total.fallbacks+=after.fallbacks-before.fallbacks;total.analyses+=after.analyses-before.analyses;
    total.nativeDecodes+=after.nativeDecodes-before.nativeDecodes;total.contourFilters+=after.contourFilters-before.contourFilters;
    total.sharpens+=after.sharpens-before.sharpens;total.referenceFilters+=after.referenceFilters-before.referenceFilters;
    total.motionReconstructions+=after.motionReconstructions-before.motionReconstructions;total.sparseCoefficientUploads+=after.sparseCoefficientUploads-before.sparseCoefficientUploads;
    total.bufferAllocations+=after.bufferAllocations-before.bufferAllocations;total.allocatedBufferBytes+=after.allocatedBufferBytes-before.allocatedBufferBytes;
}
}
