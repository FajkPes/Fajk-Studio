#include "motion_warp_avx2.hpp"
#include <immintrin.h>
namespace fajk {
uint32_t motionWarpRowAvx2(const uint8_t* source,uint8_t* output,uint32_t width,uint32_t height,uint32_t y,
    std::span<const uint32_t> vectors,std::span<const uint32_t> gradients,
    std::span<const uint8_t> modes,const std::array<int,3>& bias){
    const auto zero=_mm256_setzero_si256(),one=_mm256_set1_epi32(1),four=_mm256_set1_epi32(4),byte=_mm256_set1_epi32(255);
    const auto lanes=_mm256_setr_epi32(0,1,2,3,4,5,6,7),w=_mm256_set1_epi32(int(width));
    const auto maxX=_mm256_set1_epi32(int(width-1)*4),maxY=_mm256_set1_epi32(int(height-1)*4);
    const uint32_t cols=(width+31)/32,maskCols=(width+7)/8;
    uint32_t x=0;
    for(;x+8<=width;x+=8){
        const auto at=size_t(y/32)*cols+x/32;
        const bool stationary=!modes.empty()&&(modes[size_t(y/8)*maskCols+x/8]&2);
        const auto motion=stationary?0u:vectors[at],gradient=stationary||gradients.empty()?0u:gradients[at];
        const int vx=int(int16_t(uint16_t(motion))),vy=int(int16_t(uint16_t(motion>>16)));
        auto packed=_mm256_set1_epi32(int(0xff000000u));
        if(!motion&&!gradient){
            const auto own=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(source+(size_t(y)*width+x)*4));
            for(int c=0;c<3;++c){const auto shift=_mm256_set1_epi32(c*8);
                const auto channel=_mm256_and_si256(byte,_mm256_srlv_epi32(own,shift));
                const auto value=_mm256_max_epi32(zero,_mm256_min_epi32(byte,_mm256_add_epi32(channel,_mm256_set1_epi32(bias[c]))));
                packed=_mm256_or_si256(packed,_mm256_sllv_epi32(value,shift));}
        }else{
            auto dx=_mm256_set1_epi32(vx),dy=_mm256_set1_epi32(vy);
            if(gradient){const auto localX=_mm256_add_epi32(lanes,_mm256_set1_epi32(int(x%32)-16));const int localY=int(y%32)-16;
                auto affine=[&](unsigned a,unsigned b){
                    const int gx=int(int8_t(uint8_t(gradient>>(a*8)))),gy=int(int8_t(uint8_t(gradient>>(b*8))));
                    const auto sum=_mm256_add_epi32(_mm256_mullo_epi32(localX,_mm256_set1_epi32(gx)),_mm256_set1_epi32(gy*localY));
                    const auto amount=_mm256_srli_epi32(_mm256_add_epi32(_mm256_abs_epi32(sum),_mm256_set1_epi32(128)),8);
                    return _mm256_sign_epi32(amount,sum);
                };
                dx=_mm256_add_epi32(dx,affine(0,1));dy=_mm256_add_epi32(dy,affine(2,3));
            }
            const auto sx=_mm256_max_epi32(zero,_mm256_min_epi32(maxX,_mm256_add_epi32(_mm256_slli_epi32(_mm256_add_epi32(lanes,_mm256_set1_epi32(int(x))),2),dx)));
            const auto sy=_mm256_max_epi32(zero,_mm256_min_epi32(maxY,_mm256_add_epi32(_mm256_set1_epi32(int(y)*4),dy)));
            const auto lx=_mm256_srli_epi32(sx,2),ly=_mm256_srli_epi32(sy,2);
            const auto hx=_mm256_min_epi32(_mm256_set1_epi32(int(width)-1),_mm256_add_epi32(lx,one));
            const auto hy=_mm256_min_epi32(_mm256_set1_epi32(int(height)-1),_mm256_add_epi32(ly,one));
            const auto fx=_mm256_and_si256(sx,_mm256_set1_epi32(3)),fy=_mm256_and_si256(sy,_mm256_set1_epi32(3));
            const auto ox=_mm256_sub_epi32(four,fx),oy=_mm256_sub_epi32(four,fy);
            const auto top=_mm256_mullo_epi32(ly,w),bottom=_mm256_mullo_epi32(hy,w);
            auto gather=[&](__m256i row,__m256i column){return _mm256_i32gather_epi32(reinterpret_cast<const int*>(source),_mm256_add_epi32(row,column),4);};
            const auto a=gather(top,lx),b=gather(top,hx),c=gather(bottom,lx),d=gather(bottom,hx);
            for(int channel=0;channel<3;++channel){const auto shift=_mm256_set1_epi32(channel*8);
                auto value=[&](__m256i p){return _mm256_and_si256(byte,_mm256_srlv_epi32(p,shift));};
                const auto rowA=_mm256_add_epi32(_mm256_mullo_epi32(value(a),ox),_mm256_mullo_epi32(value(b),fx));
                const auto rowB=_mm256_add_epi32(_mm256_mullo_epi32(value(c),ox),_mm256_mullo_epi32(value(d),fx));
                const auto interpolated=_mm256_srli_epi32(_mm256_add_epi32(_mm256_set1_epi32(8),_mm256_add_epi32(_mm256_mullo_epi32(rowA,oy),_mm256_mullo_epi32(rowB,fy))),4);
                const auto result=_mm256_max_epi32(zero,_mm256_min_epi32(byte,_mm256_add_epi32(interpolated,_mm256_set1_epi32(bias[channel]))));
                packed=_mm256_or_si256(packed,_mm256_sllv_epi32(result,shift));
            }
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(output+size_t(x)*4),packed);
    }
    return x;
}
}
