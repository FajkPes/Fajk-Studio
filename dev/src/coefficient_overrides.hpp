#pragma once
#include "fajk/types.hpp"
#include <array>

namespace fajk {
// Private handoff from one synchronous key encode to its canonical reference
// reconstruction. A byte-identical native packet is required before use.
// Only the most recent pack is retained; a different selected rate candidate
// declines to the ordinary decoder instead of reusing stale coefficients.
struct CoefficientEncodeCapture {
    uint32_t width=0,height=0,fixedStep=0;
    bool valid=false,used=false;
    Bytes nativePacket;
    std::array<std::vector<float>,54> bands;
};
inline thread_local CoefficientEncodeCapture* coefficientEncodeCapture=nullptr;
struct ScopedCoefficientEncodeCapture {
    CoefficientEncodeCapture* previous;
    explicit ScopedCoefficientEncodeCapture(CoefficientEncodeCapture* value):previous(coefficientEncodeCapture){coefficientEncodeCapture=value;}
    ~ScopedCoefficientEncodeCapture(){coefficientEncodeCapture=previous;}
    ScopedCoefficientEncodeCapture(const ScopedCoefficientEncodeCapture&)=delete;
    ScopedCoefficientEncodeCapture& operator=(const ScopedCoefficientEncodeCapture&)=delete;
};
struct CoefficientBandOverrides {
    uint32_t width=0,height=0;
    std::array<std::span<const float>,54> integers;
    std::array<uint8_t,54> active{};
};
// Installed only around a synchronous independent wrapper decode. The wavelet
// caller captures this pointer before joining its band workers.
inline thread_local const CoefficientBandOverrides* coefficientBandOverrides=nullptr;
struct ScopedCoefficientBandOverrides {
    const CoefficientBandOverrides* previous;
    explicit ScopedCoefficientBandOverrides(const CoefficientBandOverrides* value):previous(coefficientBandOverrides){coefficientBandOverrides=value;}
    ~ScopedCoefficientBandOverrides(){coefficientBandOverrides=previous;}
};
}
