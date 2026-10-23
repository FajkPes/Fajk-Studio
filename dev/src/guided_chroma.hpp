#pragma once
#include <algorithm>
#include <cstdint>
#include <immintrin.h>
namespace fajk {
uint32_t guidedChromaRowAvx2(const float*,const float*,const float*,const float*,const float*,const float*,const float*,int,uint32_t,float,float*,float*,uint8_t);
// Native CfL path: same four positive bilateral weights as the frame-sized
// reference, but only two output rows are needed before conversion to RGB.
inline void nativeGuidedChromaRow(const float* luma,const float* g0,const float* g1,const float* u0,const float* u1,const float* v0,const float* v1,int last,uint32_t width,float inverseGuide,float* outU,float* outV,bool optimized=true,uint8_t model=0){
    uint32_t x=optimized?guidedChromaRowAvx2(luma,g0,g1,u0,u1,v0,v1,last,width,inverseGuide,outU,outV,model):0;
    const float basis[4]={.5625f,.1875f,.1875f,.0625f};
    for(;x<width;++x){const int cx=int(x/2),nx=std::clamp(cx+(x%2?1:-1),0,last);const float guides[4]={g0[cx],g0[nx],g1[cx],g1[nx]};float weights[4],total=0;
        for(int i=0;i<4;++i){const float d=luma[x]-guides[i];weights[i]=basis[i]/(1+d*d*inverseGuide);total+=weights[i];}
        float u=0,v=0;const float us[4]={u0[cx],u0[nx],u1[cx],u1[nx]},vs[4]={v0[cx],v0[nx],v1[cx],v1[nx]};
        for(int i=0;i<4;++i){u+=weights[i]*us[i];v+=weights[i]*vs[i];}outU[x]=u/total;outV[x]=v/total;
        if(model){float mean=0;for(int i=0;i<4;++i)mean+=weights[i]*guides[i];mean/=total;float variance=0;for(int i=0;i<4;++i){const float d=guides[i]-mean;variance+=weights[i]*d*d;}variance/=total;
            auto fit=[&](const float* values,float average){float covariance=0;for(int i=0;i<4;++i)covariance+=weights[i]*(guides[i]-mean)*(values[i]-average);covariance/=total;const auto [low,high]=std::minmax_element(values,values+4);const float correction=(model==1?.5f:model==2?1.f:1.5f)*covariance/(variance+16.f)*(luma[x]-mean);return std::clamp(average+correction,*low,*high);};
            outU[x]=fit(us,outU[x]);outV[x]=fit(vs,outV[x]);
        }
    }
}
}
