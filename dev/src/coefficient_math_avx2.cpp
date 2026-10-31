#include "coefficient_math.hpp"
#include <immintrin.h>

namespace fajk {
namespace {
template<bool difference> bool prediction(std::span<float> values,std::span<const float> reference,float ratio){
    const auto scale=_mm256_set1_ps(ratio),limit=_mm256_set1_ps(1048576.f),negative=_mm256_set1_ps(-1048576.f);
    const auto half=_mm256_set1_ps(.5f),minusHalf=_mm256_set1_ps(-.5f),absolute=_mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    auto valid=_mm256_castsi256_ps(_mm256_set1_epi32(-1));size_t i=0;
    for(;i+8<=values.size();i+=8){
        const auto scaled=_mm256_min_ps(limit,_mm256_max_ps(negative,_mm256_mul_ps(_mm256_loadu_ps(reference.data()+i),scale)));
        const auto integral=_mm256_cvttps_epi32(scaled);const auto fraction=_mm256_sub_ps(scaled,_mm256_cvtepi32_ps(integral));
        // Comparisons produce -1/0: subtract the positive half mask, add the
        // negative half mask. This preserves the scalar half-away rule exactly.
        const auto rounded=_mm256_add_epi32(_mm256_sub_epi32(integral,_mm256_castps_si256(_mm256_cmp_ps(fraction,half,_CMP_GE_OQ))),
            _mm256_castps_si256(_mm256_cmp_ps(fraction,minusHalf,_CMP_LE_OQ)));
        const auto own=_mm256_loadu_ps(values.data()+i),predicted=_mm256_cvtepi32_ps(rounded);
        const auto result=difference?_mm256_sub_ps(own,predicted):_mm256_add_ps(own,predicted);
        _mm256_storeu_ps(values.data()+i,result);
        valid=_mm256_and_ps(valid,_mm256_cmp_ps(_mm256_and_ps(result,absolute),limit,difference?_CMP_LT_OQ:_CMP_LE_OQ));
    }
    bool bounded=_mm256_movemask_ps(valid)==255;
    for(;i<values.size();++i){const auto predicted=predictCoefficient(reference[i],ratio);
        values[i]=difference?values[i]-predicted:values[i]+predicted;
        bounded&=difference?std::abs(values[i])<1048576:std::abs(values[i])<=1048576;
    }
    _mm256_zeroupper();return bounded;
}
}
bool coefficientPredictionAvx2(std::span<float> values,std::span<const float> reference,float ratio,bool difference){
    return difference?prediction<true>(values,reference,ratio):prediction<false>(values,reference,ratio);
}
void restoreCoefficientBandAvx2(const float* integers,float* output,int stride,int width,int height,float step,bool bias){
    const auto zero=_mm256_setzero_ps(),scale=_mm256_set1_ps(step),amount=_mm256_set1_ps(.15f),sign=_mm256_set1_ps(-0.f);
    for(int y=0;y<height;++y){const auto* input=integers+size_t(y)*width;auto* row=output+size_t(y)*stride;int x=0;
        for(;x+8<=width;x+=8){const auto value=_mm256_loadu_ps(input+x);
            const auto restored=bias?_mm256_add_ps(value,_mm256_or_ps(amount,_mm256_and_ps(value,sign))):value;
            _mm256_storeu_ps(row+x,_mm256_and_ps(_mm256_cmp_ps(value,zero,_CMP_NEQ_UQ),_mm256_mul_ps(restored,scale)));
        }
        for(;x<width;++x){const auto value=input[x];row[x]=value?(bias?value+std::copysign(.15f,value):value)*step:0.f;}
    }
    _mm256_zeroupper();
}
}
