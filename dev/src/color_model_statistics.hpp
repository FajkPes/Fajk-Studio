#pragma once
namespace fajk {
// Identical layouts for the CPU equations and optional GPU summaries.
struct GpuColorStatistics {
    double sum=0,square=0,color[2]{},cross[2]{};
    double detailVariance=0,detailCross[2]{};
    unsigned count=0,detailCount=0;
};
struct ColorModelPatch {
    double covariance[4][2]{},matrix[4][4]{},samples[4]{};
};
static_assert(sizeof(GpuColorStatistics)==80&&sizeof(ColorModelPatch)==224);
}
