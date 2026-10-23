namespace fajk {
static __m256i gammaByteVectorAvx2(__m256 linear,const float* table,const uint32_t* packedTable){
    const auto bits=_mm256_castps_si256(_mm256_min_ps(_mm256_set1_ps(1),_mm256_max_ps(_mm256_setzero_ps(),linear)));
    const auto index=_mm256_max_epi32(_mm256_setzero_si256(),_mm256_sub_epi32(_mm256_srli_epi32(bits,16),_mm256_set1_epi32(112*128)));
    const auto packed=_mm256_i32gather_epi32(reinterpret_cast<const int*>(packedTable),index,4),fraction=_mm256_and_si256(bits,_mm256_set1_epi32(65535));
    const auto increment=_mm256_cmpgt_epi32(fraction,_mm256_sub_epi32(_mm256_srli_epi32(packed,8),_mm256_set1_epi32(1)));
    return _mm256_sub_epi32(_mm256_and_si256(packed,_mm256_set1_epi32(255)),increment);
}
uint32_t gammaByteRowAvx2(const float* input,uint32_t width,uint32_t* output){uint32_t x=0;for(;x+8<=width;x+=8)_mm256_storeu_si256(reinterpret_cast<__m256i*>(output+x),gammaByteVectorAvx2(_mm256_loadu_ps(input+x),perceptualColor::gammaTable().data(),perceptualColor::gammaExponentByteTable().data()));return x;}
}
