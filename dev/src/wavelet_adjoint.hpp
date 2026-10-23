#pragma once
#include "wavelet.hpp"
#include "fajk/hardware.hpp"
#include <array>
#include <immintrin.h>

namespace fajk {
bool hasAvx2();
void synthesisAdjointRowAvx2(float*,float*,int);
void synthesisAdjointColumnsAvx2(float*,float*,int,int,int,int);
// Transpose of the CDF 9/7 synthesis operator, for encoder distortion gradients.
// This is not the ordinary forward wavelet. Symmetric boundary contributions
// are accumulated with their true multiplicity, including short coarse rows.
inline void synthesisAdjointRow(float* data,int count,bool allowAvx2=true){
    constexpr float alpha=-1.586134342f,beta=-.05298011854f,gamma=.8829110762f,delta=.4435068522f,normal=1.149604398f;
    thread_local std::vector<float> scratch;scratch.resize(count);
    if(allowAvx2&&count>=32&&hasAvx2()){synthesisAdjointRowAvx2(data,scratch.data(),count);return;}
    const int half=count/2;auto* low=scratch.data();auto* high=low+half;
    int i=0;for(;i+4<=half;i+=4){const auto a=_mm_loadu_ps(data+i*2),b=_mm_loadu_ps(data+i*2+4);_mm_storeu_ps(low+i,_mm_shuffle_ps(a,b,_MM_SHUFFLE(2,0,2,0)));_mm_storeu_ps(high+i,_mm_shuffle_ps(a,b,_MM_SHUFFLE(3,1,3,1)));}
    for(;i<half;++i){low[i]=data[i*2];high[i]=data[i*2+1];}
    auto add=[&](float* target,const float* source,int count,float factor){int x=0;const auto f=_mm_set1_ps(factor);for(;x+4<=count;x+=4)_mm_storeu_ps(target+x,_mm_add_ps(_mm_loadu_ps(target+x),_mm_mul_ps(f,_mm_loadu_ps(source+x))));for(;x<count;++x)target[x]+=factor*source[x];};
    auto transposeHigh=[&](float factor){
        add(low+1,high,half-1,factor);add(low,high,half,factor);low[half-1]+=factor*high[half-1];
    };
    auto transposeLow=[&](float factor){
        add(high,low,half,factor);high[0]+=factor*low[0];add(high,low+1,half-1,factor);
    };
    transposeHigh(-alpha);transposeLow(-beta);transposeHigh(-gamma);transposeLow(-delta);
    i=0;for(;i+4<=half;i+=4){_mm_storeu_ps(data+i,_mm_div_ps(_mm_loadu_ps(low+i),_mm_set1_ps(normal)));_mm_storeu_ps(data+half+i,_mm_mul_ps(_mm_loadu_ps(high+i),_mm_set1_ps(normal)));}
    for(;i<half;++i){data[i]=low[i]/normal;data[half+i]=high[i]*normal;}
}
inline void waveletSynthesisAdjoint(std::vector<float>& data,int stride,int height,uint32_t threads,bool allowAvx2=true){
    require(stride>=64&&height>=64&&stride%64==0&&height%64==0&&data.size()==size_t(stride)*height,"Invalid synthesis adjoint dimensions.");
    thread_local std::vector<float> cachedScratch;std::vector<float> ownedScratch;
    auto& scratch=data.size()<=8*1024*1024?cachedScratch:ownedScratch;
    if(scratch.size()<data.size())scratch.resize(data.size());
    const bool avx=allowAvx2&&hasAvx2();
    constexpr float alpha=-1.586134342f,beta=-.05298011854f,gamma=.8829110762f,delta=.4435068522f,normal=1.149604398f;
    for(int level=0;level<6;++level){const int width=stride>>level,rows=height>>level,half=rows/2;
        const uint32_t workers=uint64_t(width)*rows<=65536?1:threads;
        parallelFor(uint32_t((rows+15)/16),workers,[&](uint32_t job){for(int y=int(job)*16;y<std::min(rows,int(job+1)*16);++y)synthesisAdjointRow(data.data()+size_t(y)*stride,width,avx);});
        if(avx&&width>=8){parallelFor(uint32_t((width+31)/32),workers,[&](uint32_t job){const int begin=int(job)*32;synthesisAdjointColumnsAvx2(data.data(),scratch.data()+size_t(begin)*rows,stride,rows,begin,std::min(width,begin+32));});continue;}
        parallelFor(uint32_t((width+63)/64),workers,[&](uint32_t job){
            const int begin=int(job)*64,end=std::min(width,begin+64);
            for(int y=0;y<rows;++y){const int from=y<half?2*y:2*(y-half)+1;std::copy_n(data.data()+size_t(from)*stride+begin,end-begin,scratch.data()+size_t(y)*width+begin);}
            auto add=[&](int target,int source,float factor){auto* out=scratch.data()+size_t(target)*width;const auto* in=scratch.data()+size_t(source)*width;const auto f=_mm_set1_ps(factor);int x=begin;for(;x+4<=end;x+=4)_mm_storeu_ps(out+x,_mm_add_ps(_mm_loadu_ps(out+x),_mm_mul_ps(f,_mm_loadu_ps(in+x))));for(;x<end;++x)out[x]+=factor*in[x];};
            auto high=[&](float factor){for(int y=1;y<half;++y)add(y,half+y-1,factor);for(int y=0;y<half;++y)add(y,half+y,factor);add(half-1,rows-1,factor);};
            auto low=[&](float factor){for(int y=0;y<half;++y)add(half+y,y,factor);add(half,0,factor);for(int y=0;y+1<half;++y)add(half+y,y+1,factor);};
            high(-alpha);low(-beta);high(-gamma);low(-delta);
            for(int y=0;y<rows;++y){const auto* in=scratch.data()+size_t(y)*width;auto* out=data.data()+size_t(y)*stride;int x=begin;for(;x+4<=end;x+=4){const auto value=_mm_loadu_ps(in+x);_mm_storeu_ps(out+x,y<half?_mm_div_ps(value,_mm_set1_ps(normal)):_mm_mul_ps(value,_mm_set1_ps(normal)));}for(;x<end;++x)out[x]=y<half?in[x]/normal:in[x]*normal;}
        });
    }
}
// Verified interior CDF 9/7 synthesis constants. Precomputing these once in
// the research harness avoids dozens of full inverse transforms on first encode.
// Tests independently compare all entries with actual synthesis at another size.
inline const std::array<float,12>& fineSynthesisEnergies(){
    static constexpr std::array<float,12> result{1.02270019f,1.02270019f,1.0825069f,0.996814489f,0.996814609f,0.935506046f,1.09378433f,1.09378433f,1.08082509f,1.13798511f,1.13798523f,1.15588284f};return result;
}
inline const std::array<std::array<float,5>,12>& fineSynthesisCorrelations(){
    static constexpr std::array<std::array<float,5>,12> result{{
        {-0.0697753653f,0.0576513857f,0.0564653575f,-0.0532179624f,-0.0040226467f},
        {0.0576514639f,-0.0697754547f,-0.0532179587f,0.0564653687f,-0.00402265694f},
        {-0.0697753727f,-0.0697754622f,0.0564653575f,0.0564653799f,0.0048686089f},
        {-0.163217381f,0.0365853757f,0.0652536377f,-0.0576099269f,-0.00597136933f},
        {0.0365854837f,-0.16321744f,-0.0576099083f,0.0652536377f,-0.00597138889f},
        {-0.163217366f,-0.16321747f,0.0652536228f,0.0652536526f,0.0266399253f},
        {-0.155469209f,0.0246774331f,0.0613282956f,-0.0556038469f,-0.00383658102f},
        {0.0246775374f,-0.155469328f,-0.0556038432f,0.0613283105f,-0.00383660011f},
        {-0.155469224f,-0.155469313f,0.0613282882f,0.0613283105f,0.024170693f},
        {-0.145857528f,0.021036746f,0.0596631914f,-0.0547605827f,-0.00306836772f},
        {0.021036854f,-0.145857632f,-0.0547605753f,0.0596632063f,-0.00306838565f},
        {-0.145857558f,-0.145857647f,0.059663184f,0.0596632063f,0.02127444f}
    }};return result;
}
}
