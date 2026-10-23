#pragma once
#include "fajk/types.hpp"
#include <immintrin.h>
namespace fajk {
bool hasAvx2();
uint32_t r6CorrectionSpanAvx2(const float*,float*,float*,uint32_t,const float*,const float*);
inline void r6CorrectionRow(std::span<const uint8_t> map,std::span<const float> means,uint32_t tile,int cols,int rows,float y,uint32_t count,const float* luma,float* cb,float* cr,bool fast=false){
    fast=fast&&hasAvx2();
    const float center=(tile-1)*.5f,gy=(y-center)/tile;const int iy=int(std::floor(gy));const float fy=gy-iy;const int u=std::clamp(iy,0,rows-1),v=std::clamp(iy+1,0,rows-1);
    auto control=[&](int x,int c){x=std::clamp(x,0,cols-1);const size_t a=size_t(u)*cols+x,b=size_t(v)*cols+x;return c==2?means[a]*(1-fy)+means[b]*fy:(int8_t(map[a*2+c])*(1-fy)+int8_t(map[b*2+c])*fy)/16.f;};
    uint32_t x=0;while(x<count){const float gx=(x-center)/tile;const int ix=int(std::floor(gx));const uint32_t end=std::min(count,std::max(x+1,uint32_t(std::max(0.f,std::ceil((ix+1)*float(tile)+center)))));const auto begin=x;
        float initial[3],delta[3];for(int c=0;c<3;++c){const float a=control(ix,c),b=control(ix+1,c);initial[c]=a+(b-a)*(gx-ix);delta[c]=(b-a)/tile;}
        // A zero predictor is common in UI backgrounds and weakly correlated
        // colors. There is no reason to load luma or interpolate its mean here.
        if(initial[0]==0&&initial[1]==0&&delta[0]==0&&delta[1]==0){
            std::fill_n(cb+x,end-x,0.f);std::fill_n(cr+x,end-x,0.f);x=end;continue;
        }
        if(fast)x+=r6CorrectionSpanAvx2(luma+x,cb+x,cr+x,end-x,initial,delta);
        for(;x+4<=end;x+=4){const auto positions=_mm_setr_ps(float(x-begin),float(x-begin+1),float(x-begin+2),float(x-begin+3));
            const auto mean=_mm_add_ps(_mm_set1_ps(initial[2]),_mm_mul_ps(positions,_mm_set1_ps(delta[2]))),ac=_mm_sub_ps(_mm_loadu_ps(luma+x),mean);
            for(int c=0;c<2;++c)_mm_storeu_ps((c?cr:cb)+x,_mm_mul_ps(_mm_add_ps(_mm_set1_ps(initial[c]),_mm_mul_ps(positions,_mm_set1_ps(delta[c]))),ac));
        }
        for(;x<end;++x){const float ac=luma[x]-(initial[2]+delta[2]*(x-begin));cb[x]=(initial[0]+delta[0]*(x-begin))*ac;cr[x]=(initial[1]+delta[1]*(x-begin))*ac;}
    }
}
inline void r6ModelRow(std::span<const uint8_t> map,std::span<const float> means,uint32_t tile,int cols,int rows,float y,uint32_t count,int scale,float offset,float* cb,float* cr,float* mean){
    const float center=(tile-1)*.5f,gy=(y-center)/tile;const int iy=int(std::floor(gy));const float fy=gy-iy;const int u=std::clamp(iy,0,rows-1),v=std::clamp(iy+1,0,rows-1);
    auto control=[&](int x,int c){x=std::clamp(x,0,cols-1);const size_t a=size_t(u)*cols+x,b=size_t(v)*cols+x;return c==2?means[a]*(1-fy)+means[b]*fy:(int8_t(map[a*2+c])*(1-fy)+int8_t(map[b*2+c])*fy)/16.f;};
    uint32_t x=0;while(x<count){const float gx=(x*scale+offset-center)/tile;const int ix=int(std::floor(gx));const uint32_t end=std::min(count,std::max(x+1,uint32_t(std::max(0.f,std::ceil(((ix+1)*float(tile)+center-offset)/scale)))));const auto begin=x;
        float initial[3],delta[3];for(int c=0;c<3;++c){const float a=control(ix,c),b=control(ix+1,c);initial[c]=a+(b-a)*(gx-ix);delta[c]=(b-a)*scale/tile;}
        float* out[3]={cb,cr,mean};for(;x+4<=end;x+=4){const auto positions=_mm_setr_ps(float(x-begin),float(x-begin+1),float(x-begin+2),float(x-begin+3));for(int c=0;c<3;++c)_mm_storeu_ps(out[c]+x,_mm_add_ps(_mm_set1_ps(initial[c]),_mm_mul_ps(positions,_mm_set1_ps(delta[c]))));}
        for(;x<end;++x)for(int c=0;c<3;++c)out[c][x]=initial[c]+delta[c]*(x-begin);
    }
}
}
