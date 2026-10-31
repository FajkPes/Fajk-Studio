#include "predictive_color_avx2.hpp"
#include <immintrin.h>
#include <cstddef>
namespace fajk {
uint32_t predictiveColorRowAvx2(uint8_t* rgba,uint32_t width,uint32_t chromaWidth,
    const float* luma,const float* coTop,const float* coBottom,
    const float* cgTop,const float* cgBottom,const uint16_t* patterns,
    const std::array<uint8_t,64>* blendTable,uint32_t phase,float fy){
    const auto zero=_mm256_setzero_si256(),oneI=_mm256_set1_epi32(1),byte=_mm256_set1_epi32(255);
    const auto zeroF=_mm256_setzero_ps(),one=_mm256_set1_ps(1.f),half=_mm256_set1_ps(.5f),quarter=_mm256_set1_ps(.25f);
    const auto yFraction=_mm256_set1_ps(fy),yOther=_mm256_set1_ps(1-fy);
    const auto positions=_mm256_setr_epi32(0,1,2,3,4,5,6,7),last=_mm256_set1_epi32(int(chromaWidth)-1);
    uint32_t x=0;
    for(;x+8<=width;x+=8){
        // The eight phases share one 8x8 skip pattern and one table row.
        const auto weights=_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64(
            reinterpret_cast<const __m128i*>(blendTable[patterns[x/8]].data()+phase*8))));
        if(_mm256_testz_si256(_mm256_castps_si256(weights),_mm256_castps_si256(weights)))continue;
        const auto offset=_mm256_add_epi32(positions,_mm256_set1_epi32(int(x)));
        const auto low=_mm256_srli_epi32(_mm256_max_epi32(zero,_mm256_sub_epi32(offset,oneI)),1);
        const auto high=_mm256_min_epi32(last,_mm256_add_epi32(low,oneI));
        auto fx=_mm256_setr_ps(.75f,.25f,.75f,.25f,.75f,.25f,.75f,.25f);
        if(!x)fx=_mm256_blend_ps(fx,zeroF,1);
        const auto other=_mm256_sub_ps(one,fx);
        auto interpolate=[&](const float* top,const float* bottom){
            const auto a=_mm256_add_ps(_mm256_mul_ps(_mm256_i32gather_ps(top,low,4),other),_mm256_mul_ps(_mm256_i32gather_ps(top,high,4),fx));
            const auto b=_mm256_add_ps(_mm256_mul_ps(_mm256_i32gather_ps(bottom,low,4),other),_mm256_mul_ps(_mm256_i32gather_ps(bottom,high,4),fx));
            return _mm256_add_ps(_mm256_mul_ps(a,yOther),_mm256_mul_ps(b,yFraction));
        };
        const auto yy=_mm256_loadu_ps(luma+x),co=interpolate(coTop,coBottom),cg=interpolate(cgTop,cgBottom);
        const __m256 delta[3]={_mm256_sub_ps(_mm256_add_ps(yy,co),cg),_mm256_add_ps(yy,cg),_mm256_sub_ps(_mm256_sub_ps(yy,co),cg)};
        const auto own=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rgba+size_t(x)*4));
        auto packed=_mm256_and_si256(own,_mm256_set1_epi32(int(0xff000000u)));
        for(int c=0;c<3;++c){
            const auto correction=_mm256_max_ps(_mm256_set1_ps(-255.f),_mm256_min_ps(_mm256_set1_ps(255.f),_mm256_mul_ps(_mm256_mul_ps(delta[c],weights),quarter)));
            const auto magnitude=_mm256_andnot_ps(_mm256_set1_ps(-0.f),correction);
            const auto integral=_mm256_cvttps_epi32(magnitude);
            const auto remainder=_mm256_sub_ps(magnitude,_mm256_cvtepi32_ps(integral));
            const auto increment=_mm256_and_si256(oneI,_mm256_castps_si256(_mm256_cmp_ps(remainder,half,_CMP_GE_OQ)));
            const auto rounded=_mm256_add_epi32(integral,increment);
            const auto sign=_mm256_castps_si256(_mm256_cmp_ps(correction,zeroF,_CMP_LT_OQ));
            const auto signedValue=_mm256_sub_epi32(_mm256_xor_si256(rounded,sign),sign);
            const auto shift=_mm256_set1_epi32(c*8),channel=_mm256_and_si256(byte,_mm256_srlv_epi32(own,shift));
            const auto value=_mm256_min_epi32(byte,_mm256_max_epi32(zero,_mm256_add_epi32(channel,signedValue)));
            packed=_mm256_or_si256(packed,_mm256_sllv_epi32(value,shift));
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(rgba+size_t(x)*4),packed);
    }
    return x;
}
}
