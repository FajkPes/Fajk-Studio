#include "final_sharpen.hpp"
#include <immintrin.h>
namespace fajk {
void finalSharpenRowsAvx2(std::span<const uint8_t> source,std::span<uint8_t> output,uint32_t width,uint32_t height,uint32_t first,uint32_t last,uint32_t strength){
    const auto zero=_mm256_setzero_si256(),one=_mm256_set1_epi32(1),byte=_mm256_set1_epi32(255),alpha=_mm256_set1_epi32(int(0xff000000u));
    const auto gain=_mm256_set1_epi32(int((strength*384+50)/100)),blend=_mm256_set1_epi32(int((strength*256+50)/100));
    auto channel=[&](__m256i p,int shift){return _mm256_and_si256(_mm256_srlv_epi32(p,_mm256_set1_epi32(shift)),byte);};
    auto light=[&](__m256i p){return _mm256_srli_epi32(_mm256_add_epi32(_mm256_set1_epi32(2),_mm256_add_epi32(_mm256_add_epi32(channel(p,0),channel(p,16)),_mm256_slli_epi32(channel(p,8),1))),2);};
    for(uint32_t y=first;y<last;++y){
        const auto* row=source.data()+size_t(y)*width*4;const auto* up=source.data()+size_t(y?y-1:0)*width*4;const auto* down=source.data()+size_t(std::min(y+1,height-1))*width*4;
        auto scalar=[&](uint32_t x){const auto value=finalSharpPixel(source,width,height,x,y,strength);std::memcpy(output.data()+(size_t(y)*width+x)*4,&value,4);};
        scalar(0);uint32_t x=1;
        for(;x+8<width;x+=8){
            auto load=[&](const uint8_t* p){return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));};
            const auto own=load(row+x*4),north=load(up+x*4),south=load(down+x*4),west=load(row+(x-1)*4),east=load(row+(x+1)*4);
            const auto center=light(own),a=light(north),b=light(south),c=light(west),d=light(east);
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
            const auto opaque=_mm256_cmpeq_epi32(_mm256_srli_epi32(_mm256_and_si256(_mm256_and_si256(own,north),_mm256_and_si256(_mm256_and_si256(south,west),east)),24),byte);
            delta=_mm256_and_si256(delta,opaque);
            const auto packed=_mm256_or_si256(_mm256_and_si256(own,alpha),_mm256_or_si256(_mm256_add_epi32(r,delta),_mm256_or_si256(_mm256_slli_epi32(_mm256_add_epi32(g,delta),8),_mm256_slli_epi32(_mm256_add_epi32(blue,delta),16))));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output.data()+(size_t(y)*width+x)*4),packed);
        }
        for(;x<width;++x)scalar(x);
    }
}
}
