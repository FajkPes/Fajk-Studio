#pragma once
#include "fajk/types.hpp"
#include "fajk/hardware.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <immintrin.h>
#include <vector>

namespace fajk {
uint32_t waveletOklabColorRowAvx2(const float*,const float*,const float*,int,int,uint32_t,uint32_t,bool,uint32_t,uint8_t*,const float*,const float*,const float*);
}

namespace fajk::perceptualColor {
// Oklab matrices by Björn Ottosson (2021), public-domain reference:
// https://bottosson.github.io/posts/oklab/
// Stored axes are 255*L-128, 384*a, 384*b. Gamma conversion uses sRGB.
inline const std::array<float,256>& linearTable(){
    static const auto table=[] {std::array<float,256> result{};for(int i=0;i<256;++i){const float s=i/255.f;result[i]=s<=.04045f?s/12.92f:std::pow((s+.055f)/1.055f,2.4f);}return result;}();return table;
}
inline std::array<float,3> forward(const uint8_t* pixel){
    const auto& t=linearTable();const float r=t[pixel[0]],g=t[pixel[1]],b=t[pixel[2]];
    const float l=std::cbrt(.4122214708f*r+.5363325363f*g+.0514459929f*b);
    const float m=std::cbrt(.2119034982f*r+.6806995451f*g+.1073969566f*b);
    const float s=std::cbrt(.0883024619f*r+.2817188376f*g+.6299787005f*b);
    return {255*(.2104542553f*l+.7936177850f*m-.0040720468f*s)-128,
            384*(1.9779984951f*l-2.4285922050f*m+.4505937099f*s),
            384*(.0259040371f*l+.7827717662f*m-.8086757660f*s)};
}
// Four sRGB pixels, with two Newton steps for positive cube roots. The
// reference conversion remains available for precision tests and old profiles.
inline void forward4(const uint8_t* pixels,__m128& L,__m128& A,__m128& B){
    const auto& table=linearTable();alignas(16) float r[4],g[4],b[4];
    for(int i=0;i<4;++i){r[i]=table[pixels[i*4]];g[i]=table[pixels[i*4+1]];b[i]=table[pixels[i*4+2]];}
    const auto red=_mm_load_ps(r),green=_mm_load_ps(g),blue=_mm_load_ps(b);
    auto mix=[&](__m128 x,float cx,__m128 y,float cy,__m128 z,float cz){return _mm_add_ps(_mm_add_ps(_mm_mul_ps(x,_mm_set1_ps(cx)),_mm_mul_ps(y,_mm_set1_ps(cy))),_mm_mul_ps(z,_mm_set1_ps(cz)));};
    auto root=[](__m128 x){
        const auto bits=_mm_castps_si128(x);
        const auto exponent=_mm_cvttps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(bits),_mm_set1_ps(1.f/3)));
        auto y=_mm_castsi128_ps(_mm_add_epi32(exponent,_mm_set1_epi32(709921077)));
        for(int i=0;i<2;++i)y=_mm_mul_ps(_mm_add_ps(_mm_add_ps(y,y),_mm_div_ps(x,_mm_mul_ps(y,y))),_mm_set1_ps(1.f/3));
        return _mm_and_ps(y,_mm_cmpgt_ps(x,_mm_setzero_ps()));
    };
    const auto l=root(mix(red,.4122214708f,green,.5363325363f,blue,.0514459929f));
    const auto m=root(mix(red,.2119034982f,green,.6806995451f,blue,.1073969566f));
    const auto s=root(mix(red,.0883024619f,green,.2817188376f,blue,.6299787005f));
    L=_mm_sub_ps(_mm_mul_ps(mix(l,.2104542553f,m,.7936177850f,s,-.0040720468f),_mm_set1_ps(255)),_mm_set1_ps(128));
    A=_mm_mul_ps(mix(l,1.9779984951f,m,-2.4285922050f,s,.4505937099f),_mm_set1_ps(384));
    B=_mm_mul_ps(mix(l,.0259040371f,m,.7827717662f,s,-.8086757660f),_mm_set1_ps(384));
}
inline void planes(const Frame& source,std::array<std::vector<float>,3>& dst,int w,int h,int cw,int ch,bool subsampled,uint32_t threads,bool fast=false){
    std::array<std::vector<float>,2> full;for(auto& p:full)p.resize(size_t(w)*h);
    parallelFor(uint32_t(h),threads,[&](uint32_t y){int x=0;if(fast)for(;uint32_t(x+4)<=source.width;x+=4){__m128 l,a,b;forward4(source.pixels.data()+(size_t(std::min(y,source.height-1))*source.width+x)*4,l,a,b);const size_t at=size_t(y)*w+x;_mm_storeu_ps(dst[0].data()+at,l);_mm_storeu_ps(full[0].data()+at,a);_mm_storeu_ps(full[1].data()+at,b);}for(;x<w;++x){const auto value=forward(source.pixels.data()+(size_t(std::min(y,source.height-1))*source.width+std::min(uint32_t(x),source.width-1))*4);
        const size_t at=size_t(y)*w+x;dst[0][at]=value[0];full[0][at]=value[1];full[1][at]=value[2];
    }});
    if(!subsampled){dst[1]=std::move(full[0]);dst[2]=std::move(full[1]);return;}
    parallelFor(uint32_t(ch),threads,[&](uint32_t y){for(int x=0;x<cw;++x){const int ax=std::min(int(source.width)-1,x*2),bx=std::min(int(source.width)-1,ax+1),ay=std::min(int(source.height)-1,int(y)*2),by=std::min(int(source.height)-1,ay+1);
        for(int c=0;c<2;++c)dst[c+1][size_t(y)*cw+x]=((full[c][size_t(ay)*w+ax]+full[c][size_t(ay)*w+bx])+(full[c][size_t(by)*w+ax]+full[c][size_t(by)*w+bx]))*.25f;
    }});
}
inline const std::array<float,8193>& gammaTable(){
    static const auto table=[] {std::array<float,8193> result{};for(int i=0;i<=8192;++i){const float v=i/8192.f;result[i]=255*(v<=.0031308f?12.92f*v:1.055f*std::pow(v,1.f/2.4f)-.055f);}return result;}();return table;
}
// Each gamma interval crosses at most one 8-bit rounding boundary. A packed
// threshold removes the second table gather for ordinary pixels. Values near
// the quantized threshold still use the original float interpolation.
inline const std::array<uint32_t,8193>& gammaByteTable(){
    static const auto packed=[] {std::array<uint32_t,8193> result{};const auto& t=gammaTable();for(int i=0;i<=8192;++i){const int low=int(t[i]+.5f),high=int(t[std::min(8192,i+1)]+.5f);uint32_t threshold=8388608;if(low!=high){uint32_t lo=0,hi=std::bit_cast<uint32_t>(1.f);const float delta=t[i+1]-t[i];while(lo<hi){const uint32_t mid=lo+(hi-lo)/2;const float fraction=std::bit_cast<float>(mid);const float interpolated=t[i]+fraction*delta;const int rounded=int(interpolated+.5f);if(rounded>low)hi=mid;else lo=mid+1;}threshold=uint32_t(std::ceil(double(std::bit_cast<float>(lo))*8388608.));}result[i]=uint32_t(low)|(threshold<<8);}return result;}();return packed;
}
// Seven mantissa bits per exponent keep each interval narrower than one
// sRGB output step. Exact float-bit thresholds replace an 8193-entry table,
// float interpolation, and its exceptional rounding-boundary fallback.
inline const std::array<uint32_t,1921>& gammaExponentByteTable(){
    static const auto table=[] {
        std::array<uint32_t,1921> result{};const auto& gamma=gammaTable();
        auto byte=[&](uint32_t bits){const float linear=std::clamp(std::bit_cast<float>(bits),0.f,1.f),position=linear*8192;
            const auto at=uint32_t(position);const float fraction=position-at;
            return uint32_t(gamma[at]+fraction*(gamma[std::min(8192u,at+1)]-gamma[at])+.5f);};
        for(uint32_t i=0;i<result.size();++i){const uint32_t first=(i+112*128)<<16,last=first+65535;
            const auto low=byte(first),high=byte(last);require(high<=low+1,"Gamma interval spans multiple output steps");
            uint32_t threshold=65536;
            if(high!=low){uint32_t a=first,b=last;while(a<b){const auto middle=a+(b-a)/2;if(byte(middle)>low)b=middle;else a=middle+1;}threshold=a-first;}
            result[i]=low|(threshold<<8);
        }return result;
    }();return table;
}
inline __m128 gamma4(__m128 linear){
    const auto& table=gammaTable();const auto x=_mm_mul_ps(_mm_min_ps(_mm_set1_ps(1),_mm_max_ps(_mm_setzero_ps(),linear)),_mm_set1_ps(8192));
    alignas(16) int index[4];alignas(16) float positions[4],result[4];_mm_store_si128(reinterpret_cast<__m128i*>(index),_mm_cvttps_epi32(x));_mm_store_ps(positions,x);
    for(int i=0;i<4;++i){const int at=index[i];const float f=positions[i]-at;result[i]=table[at]+f*(table[std::min(8192,at+1)]-table[at]);}return _mm_load_ps(result);
}
inline void inverse4(__m128 storedL,__m128 storedA,__m128 storedB,__m128& red,__m128& green,__m128& blue){
    const auto L=_mm_mul_ps(storedL,_mm_set1_ps(1.f/255)),a=_mm_mul_ps(storedA,_mm_set1_ps(1.f/384)),b=_mm_mul_ps(storedB,_mm_set1_ps(1.f/384));
    auto combine=[&](__m128 x,float cx,__m128 y,float cy,__m128 z,float cz){return _mm_add_ps(_mm_add_ps(_mm_mul_ps(x,_mm_set1_ps(cx)),_mm_mul_ps(y,_mm_set1_ps(cy))),_mm_mul_ps(z,_mm_set1_ps(cz)));};
    auto l=combine(L,1,a,.3963377774f,b,.2158037573f),m=combine(L,1,a,-.1055613458f,b,-.0638541728f),s=combine(L,1,a,-.0894841775f,b,-1.2914855480f);
    l=_mm_mul_ps(l,_mm_mul_ps(l,l));m=_mm_mul_ps(m,_mm_mul_ps(m,m));s=_mm_mul_ps(s,_mm_mul_ps(s,s));
    red=gamma4(combine(l,4.0767416621f,m,-3.3077115913f,s,.2309699292f));
    green=gamma4(combine(l,-1.2684380046f,m,2.6097574011f,s,-.3413193965f));
    blue=gamma4(combine(l,-.0041960863f,m,-.7034186147f,s,1.7076147010f));
}
}
