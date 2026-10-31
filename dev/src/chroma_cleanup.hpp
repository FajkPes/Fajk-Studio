#pragma once
#include "fajk/hardware.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <immintrin.h>
#include <vector>
namespace fajk {
bool hasAvx2();
void recursiveLumaHorizontalAvx2(float*,const float*,int,int,int,float,float);
void recursiveLumaVerticalAvx2(float*,const float*,int,int,int,int,int,float,float);
void recursiveChromaHorizontalAvx2(float*,float*,const float*,const float*,const float*,int,int,int,float,float);
void recursiveChromaVerticalAvx2(float*,float*,const float*,const float*,const float*,int,int,int,int,int,float,float);
// Clean only a reconstructed low-frequency grid. The untouched fine bands
// retain subpixel edge/detail information during the final inverse level.
inline void recursiveLumaCleanup(std::vector<float>& data,int stride,int width,int height,uint32_t threads,uint8_t mode,bool optimized=true){
    require(mode>=1&&mode<=7&&width>0&&height>0&&stride>=width&&data.size()>=size_t(stride)*height,"Invalid recursive luma cleanup.");
    const float range=std::array<float,7>{8,16,32,4,6,8,8}[mode-1],alpha=std::array<float,7>{.6f,.75f,.85f,.6f,.5f,.35f,.8f}[mode-1];
    thread_local std::vector<float> cachedOriginal;std::vector<float> ownedOriginal;
    auto& original=size_t(width)*height<=8*1024*1024?cachedOriginal:ownedOriginal;original.resize(size_t(width)*height);
    const bool avx=optimized&&hasAvx2();
    auto snapshot=[&](int first=0){parallelFor(uint32_t(height-first),threads,[&](uint32_t rowIndex){const uint32_t y=rowIndex+first;std::copy_n(data.data()+size_t(y)*stride,width,original.data()+size_t(y)*width);});};snapshot(avx?height/8*8:0);
    auto weight=[&](size_t a,size_t b){const float delta=std::abs(original[a]-original[b]);if(delta>=4*range)return 0.f;const float gate=1+delta*delta/(range*range);return alpha/(gate*gate);};
    parallelFor(uint32_t((height+3)/4),threads,[&](uint32_t job){const int first=int(job)*4,end=std::min(height,first+4);
        if(avx&&first/8*8+8<=height){if(first%8==0)recursiveLumaHorizontalAvx2(data.data(),data.data(),stride,width,first,range,alpha);return;}
        if(end-first==4){
            const auto sign=_mm_set1_ps(-0.f),one=_mm_set1_ps(1.f),rangeSquared=_mm_set1_ps(range*range),gain=_mm_set1_ps(alpha),limit=_mm_set1_ps(4*range);
            auto load=[&](const float* p,int pitch,int x){return _mm_setr_ps(p[size_t(first)*pitch+x],p[size_t(first+1)*pitch+x],p[size_t(first+2)*pitch+x],p[size_t(first+3)*pitch+x]);};
            auto update=[&](int x,int adjacent){const auto delta=_mm_andnot_ps(sign,_mm_sub_ps(load(original.data(),width,x),load(original.data(),width,adjacent))),gate=_mm_add_ps(one,_mm_div_ps(_mm_mul_ps(delta,delta),rangeSquared)),a=_mm_and_ps(_mm_cmplt_ps(delta,limit),_mm_div_ps(gain,_mm_mul_ps(gate,gate))),center=load(data.data(),stride,x);alignas(16) float out[4];_mm_store_ps(out,_mm_add_ps(center,_mm_mul_ps(a,_mm_sub_ps(load(data.data(),stride,adjacent),center))));for(int row=0;row<4;++row)data[size_t(first+row)*stride+x]=out[row];};
            for(int x=1;x<width;++x)update(x,x-1);for(int x=width-2;x>=0;--x)update(x,x+1);
        }else for(int y=first;y<end;++y){const size_t row=size_t(y)*stride,source=size_t(y)*width;
            for(int x=1;x<width;++x){const float a=weight(source+x,source+x-1);data[row+x]+=a*(data[row+x-1]-data[row+x]);}
            for(int x=width-2;x>=0;--x){const float a=weight(source+x,source+x+1);data[row+x]+=a*(data[row+x+1]-data[row+x]);}
        }
    });snapshot();
    parallelFor(uint32_t((width+15)/16),threads,[&](uint32_t job){const int end=std::min(width,int(job+1)*16);int x=int(job)*16;
        if(avx){const int vectorEnd=x+(end-x)/8*8;recursiveLumaVerticalAvx2(data.data(),original.data(),stride,width,height,x,vectorEnd,range,alpha);x=vectorEnd;}
        const auto sign=_mm_set1_ps(-0.f),one=_mm_set1_ps(1.f),rangeSquared=_mm_set1_ps(range*range),gain=_mm_set1_ps(alpha),limit=_mm_set1_ps(4*range);
        auto weight4=[&](size_t a,size_t b){const auto delta=_mm_andnot_ps(sign,_mm_sub_ps(_mm_loadu_ps(original.data()+a),_mm_loadu_ps(original.data()+b)));const auto gate=_mm_add_ps(one,_mm_div_ps(_mm_mul_ps(delta,delta),rangeSquared));return _mm_and_ps(_mm_cmplt_ps(delta,limit),_mm_div_ps(gain,_mm_mul_ps(gate,gate)));};
        const int vectorEnd=x+(end-x)/4*4;
        // Consume the entire cache-line strip at each row before advancing.
        // Columns remain independent; their floating-point order is unchanged.
        for(int y=1;y<height;++y)for(int column=x;column<vectorEnd;column+=4){const size_t at=size_t(y)*stride+column,src=size_t(y)*width+column;const auto center=_mm_loadu_ps(data.data()+at),previous=_mm_loadu_ps(data.data()+at-stride);_mm_storeu_ps(data.data()+at,_mm_add_ps(center,_mm_mul_ps(weight4(src,src-width),_mm_sub_ps(previous,center))));}
        for(int y=height-2;y>=0;--y)for(int column=x;column<vectorEnd;column+=4){const size_t at=size_t(y)*stride+column,src=size_t(y)*width+column;const auto center=_mm_loadu_ps(data.data()+at),previous=_mm_loadu_ps(data.data()+at+stride);_mm_storeu_ps(data.data()+at,_mm_add_ps(center,_mm_mul_ps(weight4(src,src+width),_mm_sub_ps(previous,center))));}
        x=vectorEnd;
        for(;x<end;++x){
            for(int y=1;y<height;++y){const size_t at=size_t(y)*stride+x,src=size_t(y)*width+x;const float a=weight(src,src-width);data[at]+=a*(data[at-stride]-data[at]);}
            for(int y=height-2;y>=0;--y){const size_t at=size_t(y)*stride+x,src=size_t(y)*width+x;const float a=weight(src,src+width);data[at]+=a*(data[at+stride]-data[at]);}
        }
    });
}
// Symmetric recursive color smoothing. Weights read immutable neighboring
// colors and luma; strong boundaries stop propagation in both directions.
inline void recursiveChromaCleanup(std::vector<float>& u,std::vector<float>& v,const std::vector<float>& guide,int stride,int width,int height,uint32_t threads,uint8_t mode,bool optimized=true,float amount=1.f){
    require(mode>=1&&mode<=7&&width>0&&height>0&&stride>=width&&u.size()==v.size()&&(guide.empty()||guide.size()==u.size())&&u.size()>=size_t(stride)*height,"Invalid recursive chroma cleanup.");
    require(std::isfinite(amount)&&amount>=0&&amount<=1,"Invalid color cleanup amount.");if(!amount)return;
    const float range=std::array<float,7>{4,8,16,2,4,6,8}[mode-1],alpha=std::array<float,7>{.6f,.75f,.85f,.5f,.5f,.5f,.5f}[mode-1]*amount;
    thread_local std::array<std::vector<float>,2> cache;std::array<std::vector<float>,2> owned;
    auto& originals=size_t(width)*height<=8*1024*1024?cache:owned;
    auto& originalU=originals[0];auto& originalV=originals[1];originalU.resize(size_t(width)*height);originalV.resize(originalU.size());
    const bool avx=optimized&&hasAvx2();
    auto snapshot=[&](int first=0){parallelFor(uint32_t(height-first),threads,[&](uint32_t rowIndex){const uint32_t y=rowIndex+first;const size_t row=size_t(y)*stride,source=size_t(y)*width;std::copy_n(u.data()+row,width,originalU.data()+source);std::copy_n(v.data()+row,width,originalV.data()+source);});};snapshot(avx?height/8*8:0);
    auto weight=[&](size_t a,size_t b,size_t ga,size_t gb){const float delta=std::max({std::abs(originalU[a]-originalU[b]),std::abs(originalV[a]-originalV[b]),guide.empty()?0.f:.5f*std::abs(guide[ga]-guide[gb])});if(delta>=4*range)return 0.f;const float gate=1+delta*delta/(range*range);return alpha/(gate*gate);};
    parallelFor(uint32_t((height+3)/4),threads,[&](uint32_t job){const int first=int(job)*4,end=std::min(height,first+4);
        if(avx&&first/8*8+8<=height){if(first%8==0)recursiveChromaHorizontalAvx2(u.data(),v.data(),u.data(),v.data(),guide.empty()?nullptr:guide.data(),stride,width,first,range,alpha);return;}
        if(end-first==4){
            const auto sign=_mm_set1_ps(-0.f),one=_mm_set1_ps(1.f),half=_mm_set1_ps(.5f),rangeSquared=_mm_set1_ps(range*range),gain=_mm_set1_ps(alpha),limit=_mm_set1_ps(4*range);
            auto load=[&](const float* p,int pitch,int x){return _mm_setr_ps(p[size_t(first)*pitch+x],p[size_t(first+1)*pitch+x],p[size_t(first+2)*pitch+x],p[size_t(first+3)*pitch+x]);};
            auto update=[&](int x,int adjacent){
                const auto du=_mm_andnot_ps(sign,_mm_sub_ps(load(originalU.data(),width,x),load(originalU.data(),width,adjacent))),dv=_mm_andnot_ps(sign,_mm_sub_ps(load(originalV.data(),width,x),load(originalV.data(),width,adjacent))),dg=guide.empty()?_mm_setzero_ps():_mm_mul_ps(half,_mm_andnot_ps(sign,_mm_sub_ps(load(guide.data(),stride,x),load(guide.data(),stride,adjacent))));
                const auto delta=_mm_max_ps(_mm_max_ps(du,dv),dg),gate=_mm_add_ps(one,_mm_div_ps(_mm_mul_ps(delta,delta),rangeSquared)),a=_mm_and_ps(_mm_cmplt_ps(delta,limit),_mm_div_ps(gain,_mm_mul_ps(gate,gate)));
                for(auto* data:{u.data(),v.data()}){const auto center=load(data,stride,x);alignas(16) float out[4];_mm_store_ps(out,_mm_add_ps(center,_mm_mul_ps(a,_mm_sub_ps(load(data,stride,adjacent),center))));for(int row=0;row<4;++row)data[size_t(first+row)*stride+x]=out[row];}
            };
            for(int x=1;x<width;++x)update(x,x-1);for(int x=width-2;x>=0;--x)update(x,x+1);
        }else for(int y=first;y<end;++y){const size_t row=size_t(y)*stride,source=size_t(y)*width;
            for(int x=1;x<width;++x){const size_t at=row+x,src=source+x;const float a=weight(src,src-1,at,at-1);u[at]+=a*(u[at-1]-u[at]);v[at]+=a*(v[at-1]-v[at]);}
            for(int x=width-2;x>=0;--x){const size_t at=row+x,src=source+x;const float a=weight(src,src+1,at,at+1);u[at]+=a*(u[at+1]-u[at]);v[at]+=a*(v[at+1]-v[at]);}
        }
    });snapshot();
    parallelFor(uint32_t((width+15)/16),threads,[&](uint32_t job){const int end=std::min(width,int(job+1)*16);int x=int(job)*16;
        if(avx){const int vectorEnd=x+(end-x)/8*8;recursiveChromaVerticalAvx2(u.data(),v.data(),originalU.data(),originalV.data(),guide.empty()?nullptr:guide.data(),stride,width,height,x,vectorEnd,range,alpha);x=vectorEnd;}
        const auto sign=_mm_set1_ps(-0.f),one=_mm_set1_ps(1.f),half=_mm_set1_ps(.5f),rangeSquared=_mm_set1_ps(range*range),gain=_mm_set1_ps(alpha),limit=_mm_set1_ps(4*range);
        auto weight4=[&](size_t a,size_t b,size_t ga,size_t gb){const auto du=_mm_andnot_ps(sign,_mm_sub_ps(_mm_loadu_ps(originalU.data()+a),_mm_loadu_ps(originalU.data()+b))),dv=_mm_andnot_ps(sign,_mm_sub_ps(_mm_loadu_ps(originalV.data()+a),_mm_loadu_ps(originalV.data()+b))),dg=guide.empty()?_mm_setzero_ps():_mm_mul_ps(half,_mm_andnot_ps(sign,_mm_sub_ps(_mm_loadu_ps(guide.data()+ga),_mm_loadu_ps(guide.data()+gb))));const auto delta=_mm_max_ps(_mm_max_ps(du,dv),dg),gate=_mm_add_ps(one,_mm_div_ps(_mm_mul_ps(delta,delta),rangeSquared));return _mm_and_ps(_mm_cmplt_ps(delta,limit),_mm_div_ps(gain,_mm_mul_ps(gate,gate)));};
        auto update4=[&](size_t at,size_t previous,size_t src,size_t previousSource){const auto a=weight4(src,previousSource,at,previous);for(auto* data:{u.data(),v.data()}){const auto center=_mm_loadu_ps(data+at);_mm_storeu_ps(data+at,_mm_add_ps(center,_mm_mul_ps(a,_mm_sub_ps(_mm_loadu_ps(data+previous),center))));}};
        const int vectorEnd=x+(end-x)/4*4;
        for(int y=1;y<height;++y)for(int column=x;column<vectorEnd;column+=4){const size_t at=size_t(y)*stride+column,src=size_t(y)*width+column;update4(at,at-stride,src,src-width);}
        for(int y=height-2;y>=0;--y)for(int column=x;column<vectorEnd;column+=4){const size_t at=size_t(y)*stride+column,src=size_t(y)*width+column;update4(at,at+stride,src,src+width);}
        x=vectorEnd;
        for(;x<end;++x){
            for(int y=1;y<height;++y){const size_t at=size_t(y)*stride+x,src=size_t(y)*width+x;const float a=weight(src,src-width,at,at-stride);u[at]+=a*(u[at-stride]-u[at]);v[at]+=a*(v[at-stride]-v[at]);}
            for(int y=height-2;y>=0;--y){const size_t at=size_t(y)*stride+x,src=size_t(y)*width+x;const float a=weight(src,src+width,at,at+stride);u[at]+=a*(u[at+stride]-u[at]);v[at]+=a*(v[at+stride]-v[at]);}
        }
    });
}
}
