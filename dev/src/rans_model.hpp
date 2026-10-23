#pragma once
#include <bit>
#include <cstdint>
namespace fajk {
// Exact reciprocal division for the normalized 31-bit rANS state.
// Model construction pays the division once, never once per input byte.
struct RansEncoderSymbol {
    uint32_t threshold, reciprocal, bias, complement;
    unsigned shift;
    RansEncoderSymbol():RansEncoderSymbol(1,0){}
    RansEncoderSymbol(uint32_t frequency,uint32_t cumulative) {
        threshold=(1u<<19)*frequency;complement=4096-frequency;
        if(frequency==1){reciprocal=UINT32_MAX;shift=32;bias=cumulative+4095;}
        else {const auto power=std::bit_width(frequency-1);reciprocal=uint32_t(((uint64_t(1)<<(power+31))+frequency-1)/frequency);shift=power+31;bias=cumulative;}
    }
    uint32_t advance(uint32_t state) const {
        const uint32_t quotient=uint32_t((uint64_t(state)*reciprocal)>>shift);
        return state+bias+quotient*complement;
    }
};
}
