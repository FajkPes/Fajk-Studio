#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <emmintrin.h>

namespace fajk {
struct TemporalBlockMatch { bool similar=true,different=false; };
// RGBA8 rows with a shared byte pitch. The caller validates the frame bounds
// and tolerance (0..16). Alpha has zero tolerance. Unaligned SSE2 loads cover
// only complete groups of four pixels; narrow borders use the scalar tail.
inline TemporalBlockMatch compareTemporalBlock(const uint8_t* source,const uint8_t* key,
                                               size_t pitch,uint32_t width,uint32_t height,uint32_t tolerance){
    TemporalBlockMatch result;
    const auto zero=_mm_setzero_si128();
    const auto limits=_mm_set1_epi32(int(tolerance*0x00010101u));
    for(uint32_t y=0;y<height;++y){
        uint32_t x=0;
        for(;x+4<=width;x+=4){
            const auto a=_mm_loadu_si128(reinterpret_cast<const __m128i*>(source+size_t(y)*pitch+x*4));
            const auto b=_mm_loadu_si128(reinterpret_cast<const __m128i*>(key+size_t(y)*pitch+x*4));
            const auto difference=_mm_or_si128(_mm_subs_epu8(a,b),_mm_subs_epu8(b,a));
            if(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_subs_epu8(difference,limits),zero))!=0xffff)
                return {false,false};
            result.different|=_mm_movemask_epi8(_mm_cmpeq_epi8(a,b))!=0xffff;
        }
        for(;x<width;++x)for(uint32_t c=0;c<4;++c){
            const auto at=size_t(y)*pitch+x*4+c;
            const auto difference=uint32_t(std::abs(int(source[at])-int(key[at])));
            if(difference>(c==3?0:tolerance))return {false,false};
            result.different|=difference!=0;
        }
    }
    return result;
}
}
