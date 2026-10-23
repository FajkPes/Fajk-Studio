#include "fajk/pixels.hpp"
#include <emmintrin.h>
namespace fajk {
void displayPixels(std::span<const uint8_t> rgba,std::span<uint8_t> bgra){
    require(rgba.size()==bgra.size()&&rgba.size()%4==0,"Invalid display buffer.");
    const auto alphaMask=_mm_set1_epi32(int(0xff000000u)),rbMask=_mm_set1_epi32(255),gaMask=_mm_set1_epi32(int(0xff00ff00u));
    const auto zero=_mm_setzero_si128(),round=_mm_set1_epi16(128);size_t i=0;
    for(;i+16<=rgba.size();i+=16){auto v=_mm_loadu_si128(reinterpret_cast<const __m128i*>(rgba.data()+i));
        auto swapped=_mm_or_si128(_mm_and_si128(v,gaMask),_mm_or_si128(_mm_slli_epi32(_mm_and_si128(v,rbMask),16),_mm_and_si128(_mm_srli_epi32(v,16),rbMask)));
        if(_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(v,alphaMask),alphaMask))!=65535){
            auto multiply=[&](__m128i words){auto a=_mm_shufflehi_epi16(_mm_shufflelo_epi16(words,_MM_SHUFFLE(3,3,3,3)),_MM_SHUFFLE(3,3,3,3));
                auto product=_mm_add_epi16(_mm_mullo_epi16(words,a),round);return _mm_srli_epi16(_mm_add_epi16(product,_mm_srli_epi16(product,8)),8);};
            auto premultiplied=_mm_packus_epi16(multiply(_mm_unpacklo_epi8(swapped,zero)),multiply(_mm_unpackhi_epi8(swapped,zero)));
            swapped=_mm_or_si128(_mm_andnot_si128(alphaMask,premultiplied),_mm_and_si128(v,alphaMask));
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(bgra.data()+i),swapped);
    }
    for(;i<rgba.size();i+=4){const unsigned r=rgba[i],g=rgba[i+1],b=rgba[i+2],a=rgba[i+3];bgra[i]=uint8_t((b*a+127)/255);bgra[i+1]=uint8_t((g*a+127)/255);bgra[i+2]=uint8_t((r*a+127)/255);bgra[i+3]=uint8_t(a);}
}
}
