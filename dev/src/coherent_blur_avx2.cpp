#include "fajk/hardware.hpp"
#include <immintrin.h>
#include <algorithm>
namespace fajk {
// Interleaved RGB is filtered in contiguous SIMD spans. The old vertical walk
// visited one column at a time, with strided cache misses and false sharing.
void coherentBoxBlur(const float* input,float* temporary,float* output,uint32_t w,uint32_t h,int radius,uint32_t workers){
    const uint32_t stride=w*3;const auto divisor=_mm256_set1_ps(float(2*radius+1));
    parallelFor(h,workers,[&](uint32_t y){const auto* row=input+size_t(y)*stride;auto* dst=temporary+size_t(y)*stride;
        uint32_t at=0;
        for(;at<std::min(w,uint32_t(radius))*3;++at){float sum=0;for(int dx=-radius;dx<=radius;++dx)sum+=row[std::clamp(int(at/3)+dx,0,int(w)-1)*3+at%3];dst[at]=sum/(2*radius+1);}
        for(;at+8<=stride&&at+8+radius*3<=stride;at+=8){auto sum=_mm256_setzero_ps();for(int dx=-radius;dx<=radius;++dx)sum=_mm256_add_ps(sum,_mm256_loadu_ps(row+at+dx*3));_mm256_storeu_ps(dst+at,_mm256_div_ps(sum,divisor));}
        for(;at<stride;++at){float sum=0;for(int dx=-radius;dx<=radius;++dx)sum+=row[std::clamp(int(at/3)+dx,0,int(w)-1)*3+at%3];dst[at]=sum/(2*radius+1);}
    });
    parallelFor(h,workers,[&](uint32_t y){auto* dst=output+size_t(y)*stride;uint32_t x=0;
        for(;x+8<=stride;x+=8){auto sum=_mm256_setzero_ps();for(int dy=-radius;dy<=radius;++dy)sum=_mm256_add_ps(sum,_mm256_loadu_ps(temporary+size_t(std::clamp(int(y)+dy,0,int(h)-1))*stride+x));_mm256_storeu_ps(dst+x,_mm256_div_ps(sum,divisor));}
        for(;x<stride;++x){float sum=0;for(int dy=-radius;dy<=radius;++dy)sum+=temporary[size_t(std::clamp(int(y)+dy,0,int(h)-1))*stride+x];dst[x]=sum/(2*radius+1);}
    });_mm256_zeroupper();
}
void coherentTangentPass(const float* current,float* next,const float* directions,const float* detail,
                         uint32_t w,uint32_t h,uint32_t workers,float radius,float range,float stronger){
    parallelFor(h,workers,[&](uint32_t y){
        const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1),absMask=_mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
        auto abs=[&](__m256 v){return _mm256_and_ps(v,absMask);};
        auto sat=[&](__m256 v){return _mm256_min_ps(one,_mm256_max_ps(zero,v));};
        for(uint32_t x=0;x<w;x+=8){
            const uint32_t lanes=std::min(8u,w-x);const auto ix=_mm256_min_epi32(_mm256_set1_epi32(w-1),_mm256_setr_epi32(x,x+1,x+2,x+3,x+4,x+5,x+6,x+7));
            const auto at=_mm256_mullo_epi32(_mm256_add_epi32(ix,_mm256_set1_epi32(y*w)),_mm256_set1_epi32(3));
            __m256 own[3],dir[3],sum[3];for(int c=0;c<3;++c){own[c]=_mm256_i32gather_ps(current+c,at,4);dir[c]=_mm256_i32gather_ps(directions+c,at,4);sum[c]=own[c];}
            auto guard=one;if(lanes==8)guard=_mm256_loadu_ps(detail+size_t(y)*w+x);else{alignas(32)float tail[8];for(unsigned i=0;i<8;++i)tail[i]=detail[size_t(y)*w+x+std::min(i,lanes-1)];guard=_mm256_load_ps(tail);}
            const auto vx=_mm256_cvtepi32_ps(ix),vy=_mm256_set1_ps(float(y));auto weight=one;
            auto sample=[&](__m256 px,__m256 py,__m256 (&value)[3]){
                px=_mm256_min_ps(_mm256_set1_ps(float(w-1)),_mm256_max_ps(zero,px));py=_mm256_min_ps(_mm256_set1_ps(float(h-1)),_mm256_max_ps(zero,py));
                const auto ax=_mm256_cvttps_epi32(px),ay=_mm256_cvttps_epi32(py),bx=_mm256_min_epi32(_mm256_set1_epi32(w-1),_mm256_add_epi32(ax,_mm256_set1_epi32(1))),by=_mm256_min_epi32(_mm256_set1_epi32(h-1),_mm256_add_epi32(ay,_mm256_set1_epi32(1)));
                const auto tx=_mm256_sub_ps(px,_mm256_cvtepi32_ps(ax)),ty=_mm256_sub_ps(py,_mm256_cvtepi32_ps(ay)),sx=_mm256_sub_ps(one,tx),sy=_mm256_sub_ps(one,ty);
                const auto rowA=_mm256_mullo_epi32(ay,_mm256_set1_epi32(w)),rowB=_mm256_mullo_epi32(by,_mm256_set1_epi32(w)),three=_mm256_set1_epi32(3);
                const auto aa=_mm256_mullo_epi32(_mm256_add_epi32(rowA,ax),three),ab=_mm256_mullo_epi32(_mm256_add_epi32(rowA,bx),three),ba=_mm256_mullo_epi32(_mm256_add_epi32(rowB,ax),three),bb=_mm256_mullo_epi32(_mm256_add_epi32(rowB,bx),three);
                for(int c=0;c<3;++c){const auto top=_mm256_add_ps(_mm256_mul_ps(_mm256_i32gather_ps(current+c,aa,4),sx),_mm256_mul_ps(_mm256_i32gather_ps(current+c,ab,4),tx));
                    const auto bottom=_mm256_add_ps(_mm256_mul_ps(_mm256_i32gather_ps(current+c,ba,4),sx),_mm256_mul_ps(_mm256_i32gather_ps(current+c,bb,4),tx));
                    value[c]=_mm256_add_ps(_mm256_mul_ps(top,sy),_mm256_mul_ps(bottom,ty));}
            };
            for(int axis=0;axis<2;++axis)for(int step=-3;step<=3;++step){if(!step)continue;
                auto distance=_mm256_set1_ps(step*radius/3);if(axis)distance=_mm256_mul_ps(distance,_mm256_sub_ps(one,_mm256_mul_ps(_mm256_set1_ps(.85f),dir[2])));
                const auto dx=axis?_mm256_sub_ps(zero,dir[1]):dir[0],dy=axis?dir[0]:dir[1];__m256 p[3];
                sample(_mm256_add_ps(vx,_mm256_mul_ps(dx,distance)),_mm256_add_ps(vy,_mm256_mul_ps(dy,distance)),p);
                auto error=zero;for(int c=0;c<3;++c)error=_mm256_max_ps(error,abs(_mm256_sub_ps(p[c],own[c])));
                const auto localRange=axis?_mm256_set1_ps(range):_mm256_add_ps(_mm256_set1_ps(range),_mm256_mul_ps(_mm256_set1_ps(stronger*36),dir[2]));
                const auto r=_mm256_div_ps(error,localRange);auto v=_mm256_max_ps(zero,_mm256_sub_ps(one,_mm256_mul_ps(r,r)));
                v=_mm256_mul_ps(v,_mm256_mul_ps(v,_mm256_set1_ps(step*step==9?.35f:step*step==4?.7f:1.f)));
                if(stronger){v=_mm256_mul_ps(v,_mm256_sub_ps(one,_mm256_mul_ps(guard,axis?one:_mm256_sub_ps(one,dir[2]))));if(axis)v=_mm256_mul_ps(v,_mm256_sub_ps(one,_mm256_mul_ps(_mm256_set1_ps(stronger),dir[2])));}
                for(int c=0;c<3;++c)sum[c]=_mm256_add_ps(sum[c],_mm256_mul_ps(v,p[c]));weight=_mm256_add_ps(weight,v);
            }
            const auto amount=_mm256_mul_ps(_mm256_set1_ps(.65f),_mm256_sub_ps(one,_mm256_mul_ps(guard,_mm256_set1_ps(1-stronger))));
            alignas(32)float result[3][8];for(int c=0;c<3;++c)_mm256_store_ps(result[c],_mm256_add_ps(own[c],_mm256_mul_ps(amount,_mm256_sub_ps(_mm256_div_ps(sum[c],weight),own[c]))));
            for(uint32_t lane=0;lane<lanes;++lane)for(int c=0;c<3;++c)next[(size_t(y)*w+x+lane)*3+c]=result[c][lane];
        }
    });_mm256_zeroupper();
}
}
