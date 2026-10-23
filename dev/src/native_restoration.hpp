#pragma once
#include "fajk/hardware.hpp"
#include <array>
#include <immintrin.h>
#include <vector>
namespace fajk {
bool hasAvx2();
int nativeRestoreBlockRowAvx2(const float*,float*,int,int,int,int,float);
void nativeRestoreBlockAvx2(const float*,float*,int,int,int,int,int,int,float);
int nativeRestoreDirectionAvx2(const float*,int,int,int,int);
// Experimental floating-point directional reconstruction. The constrained
// difference follows the CDEF principle; this is not AV1 bitstream code.
// Read only the unfiltered plane, so tile/thread order cannot affect pixels.
inline void restoreNativePlane(std::vector<float>& plane,int stride,int width,int height,uint32_t workers,float strength,uint32_t planeIndex,bool allowAvx=true){
    if(width<8||height<8||strength<=0)return;
    thread_local std::array<std::vector<float>,3> cache;std::vector<float> owned;
    auto& restored=plane.size()<=16*1024*1024?cache[planeIndex]:owned;
    restored.resize(plane.size());
    std::copy(plane.begin()+size_t(height)*stride,plane.end(),restored.begin()+size_t(height)*stride);
    const bool avx=allowAvx&&hasAvx2();
    parallelFor(uint32_t((height+7)/8),workers,[&](uint32_t job){
        // Interior samples are all written by the filter below. Copy only
        // untouched image borders and stride padding, not the whole plane twice.
        for(int y=int(job)*8;y<std::min(height,int(job+1)*8);++y){
            const size_t row=size_t(y)*stride;
            if(y<2||y>=height-2)std::copy_n(plane.data()+row,stride,restored.data()+row);
            else {std::copy_n(plane.data()+row,2,restored.data()+row);
                std::copy(plane.begin()+row+width-2,plane.begin()+row+stride,restored.begin()+row+width-2);}
        }
        const int top=std::max(2,int(job)*8),bottom=std::min(height-2,int(job+1)*8);
        const int offsets[4]={1,stride,stride+1,stride-1};
        const auto zero=_mm_setzero_ps(),sign=_mm_set1_ps(-0.f),quarter=_mm_set1_ps(.25f);
        auto constrain=[&](__m128 difference,float limit){
            const auto magnitude=_mm_andnot_ps(sign,difference);
            const auto allowed=_mm_max_ps(zero,_mm_sub_ps(_mm_set1_ps(limit),_mm_mul_ps(magnitude,quarter)));
            return _mm_or_ps(_mm_min_ps(magnitude,allowed),_mm_and_ps(sign,difference));
        };
        for(int tx=0;tx<width;tx+=8){const int left=std::max(2,tx),right=std::min(width-2,tx+8);float costs[4]{};
            if(avx&&right-left==8){const int direction=nativeRestoreDirectionAvx2(plane.data(),stride,top,bottom,left);
                nativeRestoreBlockAvx2(plane.data(),restored.data(),stride,top,bottom,left,offsets[direction],offsets[(direction+2)%4],strength);continue;
            }
            for(int d=0;d<4;++d){auto energy=zero;float tail=0;
                for(int y=top;y<bottom;y+=2){int x=left;for(;x+4<=right;x+=4){const size_t at=size_t(y)*stride+x;const auto delta=_mm_sub_ps(_mm_loadu_ps(plane.data()+at+offsets[d]),_mm_loadu_ps(plane.data()+at-offsets[d]));energy=_mm_add_ps(energy,_mm_mul_ps(delta,delta));}for(;x<right;++x){const size_t at=size_t(y)*stride+x;const float delta=plane[at+offsets[d]]-plane[at-offsets[d]];tail+=delta*delta;}}
                alignas(16) float lanes[4];_mm_store_ps(lanes,energy);costs[d]=((lanes[0]+lanes[1])+(lanes[2]+lanes[3]))+tail;
            }
            int direction=0;for(int d=1;d<4;++d)if(costs[d]<costs[direction])direction=d;
            const int primary=offsets[direction],secondary=offsets[(direction+2)%4];
            for(int y=top;y<bottom;++y){const size_t row=size_t(y)*stride;int x=avx?nativeRestoreBlockRowAvx2(plane.data()+row,restored.data()+row,left,right,primary,secondary,strength):left;for(;x+4<=right;x+=4){const size_t at=size_t(y)*stride+x;const auto center=_mm_loadu_ps(plane.data()+at);auto sum=zero;
                for(int side:{-1,1}){
                    sum=_mm_add_ps(sum,_mm_mul_ps(constrain(_mm_sub_ps(_mm_loadu_ps(plane.data()+at+side*primary),center),strength),_mm_set1_ps(3.f/16)));
                    sum=_mm_add_ps(sum,_mm_mul_ps(constrain(_mm_sub_ps(_mm_loadu_ps(plane.data()+at+side*2*primary),center),strength),_mm_set1_ps(2.f/16)));
                    sum=_mm_add_ps(sum,_mm_mul_ps(constrain(_mm_sub_ps(_mm_loadu_ps(plane.data()+at+side*secondary),center),strength*.5f),_mm_set1_ps(1.f/16)));
                }
                _mm_storeu_ps(restored.data()+at,_mm_add_ps(center,sum));
            }
            for(;x<right;++x){const size_t at=size_t(y)*stride+x;const float center=plane[at];float sum=0;auto tap=[&](int offset,float weight,float limit){const float delta=plane[at+offset]-center,magnitude=std::abs(delta);sum+=weight*std::copysign(std::min(magnitude,std::max(0.f,limit-magnitude*.25f)),delta);};for(int side:{-1,1}){tap(side*primary,3.f/16,strength);tap(side*2*primary,2.f/16,strength);tap(side*secondary,1.f/16,strength*.5f);}restored[at]=center+sum;}
            }
        }
    });plane.swap(restored);
}
}
