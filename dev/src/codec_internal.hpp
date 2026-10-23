#pragma once
#include "fajk/codec.hpp"
#include "fajk/entropy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <optional>
#include <immintrin.h>

namespace fajk {
namespace {
constexpr uint32_t frameMagic=0x334b4a46; // FJK3
constexpr int streamCount=17;
constexpr auto expandedBits=[] {std::array<uint64_t,256> table{};for(unsigned v=0;v<256;++v)for(unsigned b=0;b<8;++b)table[v]|=uint64_t((v>>b)&1)<<(b*8);return table;}();
int medianPredict(int a,int b,int c){return std::clamp(a+b-c,std::min(a,b),std::max(a,b));}
void appendStream(ByteWriter& out,std::span<const uint8_t> src,bool raw=false){
    if(raw){out.u32(uint32_t(src.size()+5));out.u8(0);out.u32(uint32_t(src.size()));out.bytes(src);}
    else{auto coded=entropyEncode(src);out.u32(uint32_t(coded.size()));out.bytes(coded);}
}
Bytes readStream(ByteReader& in,size_t cap){const auto size=in.u32();return entropyDecode(in.bytes(size),cap);}
// Byte-domain green decorrelation is exactly reversible, including alpha.
Bytes predictive(const Frame& f,int c){
    const int w=int(f.width),h=int(f.height);Bytes residual(size_t(w)*h);
    if(c==3){bool opaque=true;const auto mask=_mm_set1_epi32(int(0xff000000u));size_t i=0;
        for(;i+16<=f.pixels.size();i+=16){auto v=_mm_loadu_si128(reinterpret_cast<const __m128i*>(f.pixels.data()+i));if(_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(v,mask),mask))!=65535){opaque=false;break;}}
        if(opaque){for(;i<f.pixels.size();i+=4)if(f.pixels[i+3]!=255){opaque=false;break;}if(opaque){residual[0]=255;return residual;}}
    }
std::vector<uint8_t> prev(w),row(w);
    for(int y=0;y<h;++y){for(int x=0;x<w;++x){const auto* px=&f.pixels[(size_t(y)*w+x)*4];
        const uint8_t v=c==0?px[1]:c==1?uint8_t(px[0]-px[1]):c==2?uint8_t(px[2]-px[1]):px[3];
        int a=x?row[x-1]:0,b=prev[x],ul=x?prev[x-1]:0;
        residual[size_t(y)*w+x]=uint8_t(v-medianPredict(a,b,ul));row[x]=v;}
        std::swap(row,prev);
    }return residual;
}
void undoPredict(std::span<const uint8_t> data,std::span<uint8_t> pixels,uint32_t width,uint32_t height,int c){
    const int w=int(width),h=int(height);require(data.size()==size_t(w)*h,"Invalid predictive plane.");
    if(c==3&&data[0]==255&&std::all_of(data.begin()+1,data.end(),[](uint8_t v){return v==0;})){
        for(size_t i=3;i<pixels.size();i+=4)pixels[i]=255;return;
    }
    std::vector<uint8_t> prev(w),row(w);
    for(int y=0;y<h;++y){for(int x=0;x<w;++x){int a=x?row[x-1]:0,b=prev[x],ul=x?prev[x-1]:0;
        const auto v=uint8_t(data[size_t(y)*w+x]+medianPredict(a,b,ul));row[x]=v;auto* px=&pixels[(size_t(y)*w+x)*4];
        if(c==0)px[1]=v;else if(c==1)px[0]=uint8_t(v+px[1]);else if(c==2)px[2]=uint8_t(v+px[1]);else px[3]=v;}
        std::swap(row,prev);
    }
}

}
}
