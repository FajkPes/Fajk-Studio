#include "final_sharpen.hpp"
#include <immintrin.h>
namespace fajk {
void finalSharpenLumaRowAvx2(const uint8_t* source,uint8_t* output,uint32_t width){
    const auto byte=_mm256_set1_epi32(255),alpha=_mm256_set1_epi32(int(0xff000000u));
    uint32_t x=0;
    for(;x+8<=width;x+=8){const auto own=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(source+size_t(x)*4));
        const auto red=_mm256_and_si256(own,byte),green=_mm256_and_si256(_mm256_srli_epi32(own,8),byte),blue=_mm256_and_si256(_mm256_srli_epi32(own,16),byte);
        const auto light=_mm256_srli_epi32(_mm256_add_epi32(_mm256_set1_epi32(2),_mm256_add_epi32(_mm256_add_epi32(red,blue),_mm256_slli_epi32(green,1))),2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(output+size_t(x)*4),_mm256_or_si256(light,_mm256_and_si256(own,alpha)));}
    for(;x<width;++x){uint32_t own;std::memcpy(&own,source+size_t(x)*4,4);
        const auto value=(own&0xff000000u)|(((own&255)+2*((own>>8)&255)+((own>>16)&255)+2)>>2);
        std::memcpy(output+size_t(x)*4,&value,4);}
}
void finalSharpenRowAvx2(const uint8_t* rgba,const uint8_t* row,const uint8_t* up,const uint8_t* down,uint8_t* output,uint32_t width,uint32_t strength){
    const auto zero=_mm256_setzero_si256(),one=_mm256_set1_epi32(1),byte=_mm256_set1_epi32(255),alpha=_mm256_set1_epi32(int(0xff000000u));
    const auto gain=_mm256_set1_epi32(int((strength*384+50)/100)),blend=_mm256_set1_epi32(int((strength*256+50)/100));
    auto channel=[&](__m256i p,int shift){return _mm256_and_si256(_mm256_srlv_epi32(p,_mm256_set1_epi32(shift)),byte);};
    auto scalar=[&](uint32_t x){auto load=[](const uint8_t* p,uint32_t at){uint32_t value;std::memcpy(&value,p+size_t(at)*4,4);return value;};
        auto value=load(rgba,x);const auto own=load(row,x),a=load(up,x),b=load(down,x),c=load(row,x?x-1:0),d=load(row,std::min(x+1,width-1));
        if(((own&a&b&c&d)>>24)==255)value=finalSharpLumaPixel(value,own&255,a&255,b&255,c&255,d&255,strength);
        std::memcpy(output+size_t(x)*4,&value,4);};
    scalar(0);uint32_t x=1;
    for(;x+8<width;x+=8){
        auto load=[&](const uint8_t* p){return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));};
        const auto own=load(rgba+x*4),ownLight=load(row+x*4),north=load(up+x*4),south=load(down+x*4),west=load(row+(x-1)*4),east=load(row+(x+1)*4);
        const auto center=_mm256_and_si256(ownLight,byte),a=_mm256_and_si256(north,byte),b=_mm256_and_si256(south,byte),c=_mm256_and_si256(west,byte),d=_mm256_and_si256(east,byte);
        const auto low=_mm256_min_epi32(center,_mm256_min_epi32(_mm256_min_epi32(a,b),_mm256_min_epi32(c,d)));
        const auto high=_mm256_max_epi32(center,_mm256_max_epi32(_mm256_max_epi32(a,b),_mm256_max_epi32(c,d)));
        const auto detail=_mm256_sub_epi32(_mm256_slli_epi32(center,2),_mm256_add_epi32(_mm256_add_epi32(a,b),_mm256_add_epi32(c,d)));
        const auto magnitude=_mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(_mm256_abs_epi32(detail),gain),_mm256_set1_epi32(512)),10);
        auto limit=_mm256_min_epi32(_mm256_set1_epi32(24),_mm256_max_epi32(one,_mm256_srli_epi32(_mm256_add_epi32(_mm256_sub_epi32(high,low),_mm256_set1_epi32(4)),2)));
        limit=_mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(limit,blend),_mm256_set1_epi32(128)),8);
        auto delta=_mm256_sign_epi32(_mm256_min_epi32(magnitude,limit),detail);
        const auto excess=_mm256_max_epi32(zero,_mm256_sub_epi32(_mm256_sub_epi32(_mm256_slli_epi32(_mm256_abs_epi32(detail),1),_mm256_sub_epi32(high,low)),_mm256_set1_epi32(4)));
        const auto attenuation=_mm256_max_epi32(_mm256_set1_epi32(64),_mm256_sub_epi32(_mm256_set1_epi32(256),_mm256_slli_epi32(excess,2)));
        delta=_mm256_sign_epi32(_mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(_mm256_abs_epi32(delta),attenuation),_mm256_set1_epi32(128)),8),delta);
        const auto r=channel(own,0),g=channel(own,8),blue=channel(own,16);
        const auto roomDown=_mm256_min_epi32(r,_mm256_min_epi32(g,blue)),roomUp=_mm256_sub_epi32(byte,_mm256_max_epi32(r,_mm256_max_epi32(g,blue)));
        delta=_mm256_max_epi32(_mm256_sub_epi32(zero,roomDown),_mm256_min_epi32(roomUp,delta));
        const auto opaque=_mm256_cmpeq_epi32(_mm256_srli_epi32(_mm256_and_si256(_mm256_and_si256(ownLight,north),_mm256_and_si256(_mm256_and_si256(south,west),east)),24),byte);
        delta=_mm256_and_si256(delta,opaque);
        const auto packed=_mm256_or_si256(_mm256_and_si256(own,alpha),_mm256_or_si256(_mm256_add_epi32(r,delta),_mm256_or_si256(_mm256_slli_epi32(_mm256_add_epi32(g,delta),8),_mm256_slli_epi32(_mm256_add_epi32(blue,delta),16))));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(output+size_t(x)*4),packed);
    }
    for(;x<width;++x)scalar(x);
}
}
