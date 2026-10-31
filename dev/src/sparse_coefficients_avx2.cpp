#include "sparse_coefficients_avx2.hpp"
#include <immintrin.h>
#include <bit>
namespace fajk {
bool appendSparseCoefficientsAvx2(std::span<const float> samples,uint32_t offset,size_t limit,std::vector<SparseCoefficient>& entries){
    size_t i=0;
    for(;i+8<=samples.size();i+=8){
        const auto bits=_mm256_castps_si256(_mm256_loadu_ps(samples.data()+i));
        unsigned nonzero=unsigned(~_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(bits,_mm256_setzero_si256()))))&255;
        if(entries.size()+std::popcount(nonzero)>limit)return false;
        while(nonzero){const unsigned lane=std::countr_zero(nonzero);entries.push_back({offset+uint32_t(i+lane),std::bit_cast<uint32_t>(samples[i+lane])});nonzero&=nonzero-1;}
    }
    for(;i<samples.size();++i)if(const auto bits=std::bit_cast<uint32_t>(samples[i]);bits){
        if(entries.size()==limit)return false;entries.push_back({offset+uint32_t(i),bits});
    }
    return true;
}
}
