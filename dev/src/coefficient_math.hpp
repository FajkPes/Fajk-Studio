#pragma once
#include "fajk/types.hpp"
#include <algorithm>
#include <cmath>
#include <span>

namespace fajk {
bool hasAvx2();
inline int predictCoefficient(float integer,float ratio){
    const float value=std::clamp(integer*ratio,-1048576.f,1048576.f);
    const int integral=int(value);const float fraction=value-integral;
    return integral+int(fraction>=.5f)-int(fraction<=-.5f);
}
bool coefficientPredictionAvx2(std::span<float> values,std::span<const float> reference,float ratio,bool difference);
inline bool coefficientPrediction(std::span<float> values,std::span<const float> reference,float ratio,bool difference=false){
    require(values.size()==reference.size(),"Coefficient prediction dimensions differ.");
    if(hasAvx2())return coefficientPredictionAvx2(values,reference,ratio,difference);
    bool bounded=true;
    for(size_t i=0;i<values.size();++i){const auto predicted=predictCoefficient(reference[i],ratio);
        values[i]=difference?values[i]-predicted:values[i]+predicted;
        bounded&=difference?std::abs(values[i])<1048576:std::abs(values[i])<=1048576;
    }
    return bounded;
}
// Writes every logical sample, including positive zero for omitted values.
void restoreCoefficientBandAvx2(const float* integers,float* output,int stride,int width,int height,float step,bool bias);
}
