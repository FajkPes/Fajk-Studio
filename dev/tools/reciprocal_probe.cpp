#include <immintrin.h>
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>

int main(){
    alignas(32) uint32_t values[8];uint32_t previous=0,changes=0;unsigned ignored=23;
    for(uint32_t base=0;base<(1u<<23);base+=8){
        const auto input=_mm256_setr_epi32(0x3f800000+base,0x3f800001+base,0x3f800002+base,0x3f800003+base,
                                         0x3f800004+base,0x3f800005+base,0x3f800006+base,0x3f800007+base);
        const auto result=_mm256_castps_si256(_mm256_rcp_ps(_mm256_castsi256_ps(input)));
        _mm256_store_si256(reinterpret_cast<__m256i*>(values),result);
        for(uint32_t lane=0;lane<8;++lane){const auto index=base+lane;
            if(index&&values[lane]!=previous){ignored=std::min(ignored,unsigned(std::countr_zero(index)));++changes;}
            previous=values[lane];}
    }
    uint32_t mismatches=0;
    for(int exponent=-10;exponent<=28;++exponent)for(uint32_t sample=0;sample<8192;++sample){
        const auto mantissa=(sample*1051u)&0x7fffffu;
        const float unit=std::bit_cast<float>(0x3f800000u+mantissa);
        const float value=std::ldexp(unit,exponent);
        const float own=_mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(value)));
        const float normalized=_mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(unit)));
        mismatches+=std::bit_cast<uint32_t>(own)!=std::bit_cast<uint32_t>(std::ldexp(normalized,-exponent));
    }
    std::cout<<"{\"mantissas_checked\":8388608,\"ignored_input_bits\":"<<ignored
             <<",\"table_entries\":"<<(1u<<(23-ignored))<<",\"output_changes\":"<<changes
             <<",\"exponent_checks\":"<<39*8192<<",\"exponent_mismatches\":"<<mismatches<<"}\n";
    return mismatches?1:0;
}
