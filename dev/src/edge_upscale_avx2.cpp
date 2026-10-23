// Polynomial radial kernel adapted from AMD FidelityFX FSR1 EASU (MIT):
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
namespace fajk {
template<bool cubic,bool fast=false>
void edgeUpscaleRowImpl(const uint8_t* source,uint32_t iw,uint32_t ih,uint8_t* target,uint32_t width,const int* low,const float* fractions,int iy,float fy,float amount){
    const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1),half=_mm256_set1_ps(.5f),fy8=_mm256_set1_ps(fy);
    auto clamp=[&](__m256 x){return _mm256_max_ps(zero,_mm256_min_ps(one,x));};
    for(uint32_t x=0;x<width;x+=8){const auto lanes=std::min(8u,width-x);alignas(32) int xx[8];alignas(32) float ff[8];
        // Cached axes are already contiguous. Only the final partial vector
        // needs replicated lanes to keep every gather inside the source image.
        if(lanes<8)for(uint32_t k=0;k<8;++k){xx[k]=low[x+std::min(k,lanes-1)];ff[k]=fractions[x+std::min(k,lanes-1)];}
        const auto lx=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lanes==8?low+x:xx));const auto fx=_mm256_loadu_ps(lanes==8?fractions+x:ff);
        auto load=[&](int dx,int dy){const auto column=_mm256_max_epi32(_mm256_setzero_si256(),_mm256_min_epi32(_mm256_set1_epi32(int(iw)-1),_mm256_add_epi32(lx,_mm256_set1_epi32(dx))));const auto* row=reinterpret_cast<const int*>(source+size_t(std::clamp(iy+dy,0,int(ih)-1))*iw*4);
            // Upscaling repeats adjacent input positions. One contiguous load
            // plus a register permutation replaces eight scalar gather loads.
            const int first=_mm256_extract_epi32(column,0),last=_mm256_extract_epi32(column,7);
            const int begin=std::min(first,int(iw)-8);
            if(iw>=8&&last-begin<8){const auto block=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(row+begin));return _mm256_permutevar8x32_epi32(block,_mm256_sub_epi32(column,_mm256_set1_epi32(begin)));}
            return _mm256_i32gather_epi32(row,column,4);};
        auto channel=[](__m256i p,int c){return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(p,_mm256_set1_epi32(c*8)),_mm256_set1_epi32(255)));};
        const auto a=load(0,0),b=load(1,0),c=load(0,1),d=load(1,1);__m256 base[3],minimum[3],maximum[3];auto xxv=zero,yyv=zero,xyv=zero;
        for(int ch=0;ch<3;++ch){const auto av=channel(a,ch),bv=channel(b,ch),cv=channel(c,ch),dv=channel(d,ch);const auto upper=_mm256_add_ps(av,_mm256_mul_ps(_mm256_sub_ps(bv,av),fx)),lower=_mm256_add_ps(cv,_mm256_mul_ps(_mm256_sub_ps(dv,cv),fx));base[ch]=_mm256_add_ps(upper,_mm256_mul_ps(_mm256_sub_ps(lower,upper),fy8));minimum[ch]=_mm256_min_ps(_mm256_min_ps(av,bv),_mm256_min_ps(cv,dv));maximum[ch]=_mm256_max_ps(_mm256_max_ps(av,bv),_mm256_max_ps(cv,dv));
            const auto gx=_mm256_add_ps(_mm256_sub_ps(bv,av),_mm256_sub_ps(dv,cv)),gy=_mm256_add_ps(_mm256_sub_ps(cv,av),_mm256_sub_ps(dv,bv));xxv=_mm256_add_ps(xxv,_mm256_mul_ps(gx,gx));yyv=_mm256_add_ps(yyv,_mm256_mul_ps(gy,gy));xyv=_mm256_add_ps(xyv,_mm256_mul_ps(gx,gy));}

        if constexpr(cubic){
            // Continuous Hermite surface, using the same twelve-neighbor support
            // as the directional kernel. Local extrema bound every output.
            const auto l0=load(-1,0),l1=load(-1,1),r0=load(2,0),r1=load(2,1),u0=load(0,-1),u1=load(1,-1),v0=load(0,2),v1=load(1,2);
            const auto tx2=_mm256_mul_ps(fx,fx),tx3=_mm256_mul_ps(tx2,fx);
            const auto hx0=_mm256_add_ps(one,_mm256_sub_ps(_mm256_add_ps(tx3,tx3),_mm256_mul_ps(_mm256_set1_ps(3),tx2))),hx1=_mm256_sub_ps(one,hx0),hx2=_mm256_add_ps(fx,_mm256_sub_ps(tx3,_mm256_add_ps(tx2,tx2))),hx3=_mm256_sub_ps(tx3,tx2);
            const float fy2=fy*fy,fy3=fy2*fy;const auto hy0=_mm256_set1_ps(2*fy3-3*fy2+1),hy1=_mm256_set1_ps(-2*fy3+3*fy2),hy2=_mm256_set1_ps(fy3-2*fy2+fy),hy3=_mm256_set1_ps(fy3-fy2);
            for(int ch=0;ch<3;++ch){const auto av=channel(a,ch),bv=channel(b,ch),cv=channel(c,ch),dv=channel(d,ch);
                const auto upper=_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(hx0,av),_mm256_mul_ps(hx1,bv)),_mm256_mul_ps(half,_mm256_add_ps(_mm256_mul_ps(hx2,_mm256_sub_ps(bv,channel(l0,ch))),_mm256_mul_ps(hx3,_mm256_sub_ps(channel(r0,ch),av)))));
                const auto lower=_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(hx0,cv),_mm256_mul_ps(hx1,dv)),_mm256_mul_ps(half,_mm256_add_ps(_mm256_mul_ps(hx2,_mm256_sub_ps(dv,channel(l1,ch))),_mm256_mul_ps(hx3,_mm256_sub_ps(channel(r1,ch),cv)))));
                const auto dy0a=_mm256_sub_ps(cv,channel(u0,ch)),dy0b=_mm256_sub_ps(dv,channel(u1,ch)),dy1a=_mm256_sub_ps(channel(v0,ch),av),dy1b=_mm256_sub_ps(channel(v1,ch),bv);
                const auto dy0=_mm256_mul_ps(half,_mm256_add_ps(dy0a,_mm256_mul_ps(fx,_mm256_sub_ps(dy0b,dy0a)))),dy1=_mm256_mul_ps(half,_mm256_add_ps(dy1a,_mm256_mul_ps(fx,_mm256_sub_ps(dy1b,dy1a))));
                const auto result=_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(hy0,upper),_mm256_mul_ps(hy1,lower)),_mm256_add_ps(_mm256_mul_ps(hy2,dy0),_mm256_mul_ps(hy3,dy1)));
                const auto bounded=_mm256_min_ps(maximum[ch],_mm256_max_ps(minimum[ch],result));base[ch]=_mm256_add_ps(base[ch],_mm256_mul_ps(_mm256_set1_ps(amount),_mm256_sub_ps(bounded,base[ch])));
            }
        }
        const auto energy=_mm256_add_ps(xxv,yyv);
        // The original confidence is exactly zero below this energy. Avoid
        // square roots, division and directional work for such whole vectors.
        if constexpr(!fast)if(amount>0&&_mm256_movemask_ps(_mm256_cmp_ps(energy,_mm256_set1_ps(1600),_CMP_GT_OQ))){
        const auto diff=_mm256_sub_ps(xxv,yyv);const auto disc=_mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(diff,diff),_mm256_mul_ps(_mm256_set1_ps(4),_mm256_mul_ps(xyv,xyv))));
        const auto confidence=_mm256_mul_ps(clamp(_mm256_mul_ps(_mm256_sub_ps(_mm256_div_ps(disc,_mm256_max_ps(one,energy)),_mm256_set1_ps(.8f)),_mm256_set1_ps(5))),clamp(_mm256_mul_ps(_mm256_sub_ps(energy,_mm256_set1_ps(1600)),_mm256_set1_ps(1.f/4800))));
        if(amount>0&&_mm256_movemask_ps(_mm256_cmp_ps(confidence,zero,_CMP_GT_OQ))){
            const auto dominant=_mm256_cmp_ps(xxv,yyv,_CMP_GE_OQ);auto nx=_mm256_blendv_ps(_mm256_add_ps(xyv,xyv),_mm256_add_ps(diff,disc),dominant),ny=_mm256_blendv_ps(_mm256_sub_ps(disc,diff),_mm256_add_ps(xyv,xyv),dominant);const auto norm=_mm256_sqrt_ps(_mm256_max_ps(one,_mm256_add_ps(_mm256_mul_ps(nx,nx),_mm256_mul_ps(ny,ny))));nx=_mm256_div_ps(nx,norm);ny=_mm256_div_ps(ny,norm);
            const auto absMask=_mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));const auto stretch=_mm256_div_ps(one,_mm256_max_ps(_mm256_set1_ps(.707106f),_mm256_max_ps(_mm256_and_ps(nx,absMask),_mm256_and_ps(ny,absMask))));const auto normal=_mm256_add_ps(one,_mm256_mul_ps(_mm256_sub_ps(stretch,one),confidence)),tangent=_mm256_sub_ps(one,_mm256_mul_ps(half,confidence)),lobe=_mm256_sub_ps(half,_mm256_mul_ps(_mm256_set1_ps(.29f),confidence)),clip=_mm256_div_ps(one,lobe);
            __m256 sum[3]={zero,zero,zero};auto weights=zero;constexpr int dx[12]={0,1,-1,0,1,2,-1,0,1,2,0,1},dy[12]={-1,-1,0,0,0,0,1,1,1,1,2,2};
            for(int tap=0;tap<12;++tap){const auto ox=_mm256_sub_ps(_mm256_set1_ps(float(dx[tap])),fx),oy=_mm256_set1_ps(float(dy[tap])-fy);const auto u=_mm256_mul_ps(_mm256_add_ps(_mm256_mul_ps(ox,nx),_mm256_mul_ps(oy,ny)),normal),v=_mm256_mul_ps(_mm256_sub_ps(_mm256_mul_ps(oy,nx),_mm256_mul_ps(ox,ny)),tangent);const auto distance=_mm256_min_ps(clip,_mm256_add_ps(_mm256_mul_ps(u,u),_mm256_mul_ps(v,v)));const auto wa=_mm256_sub_ps(_mm256_mul_ps(lobe,distance),one),wb=_mm256_sub_ps(_mm256_mul_ps(_mm256_set1_ps(.4f),distance),one);const auto weight=_mm256_mul_ps(_mm256_mul_ps(wa,wa),_mm256_sub_ps(_mm256_mul_ps(_mm256_set1_ps(1.5625f),_mm256_mul_ps(wb,wb)),_mm256_set1_ps(.5625f)));const auto p=tap==3?a:tap==4?b:tap==7?c:tap==8?d:load(dx[tap],dy[tap]);weights=_mm256_add_ps(weights,weight);for(int ch=0;ch<3;++ch)sum[ch]=_mm256_add_ps(sum[ch],_mm256_mul_ps(channel(p,ch),weight));}
            for(int ch=0;ch<3;++ch){const auto candidate=_mm256_max_ps(minimum[ch],_mm256_min_ps(maximum[ch],_mm256_div_ps(sum[ch],_mm256_max_ps(_mm256_set1_ps(.0001f),weights))));base[ch]=_mm256_add_ps(base[ch],_mm256_mul_ps(_mm256_sub_ps(candidate,base[ch]),_mm256_mul_ps(_mm256_set1_ps(amount),confidence)));}
        }
        }
        if constexpr(fast)if(amount>0){
            auto lowPixel=a,highPixel=a;
            // The four outer corners provide symmetric support around the
            // bilinear cell. Select the strongest local color channel once;
            // do not repeat an RGB min/max search over all outer samples.
            auto best=_mm256_sub_ps(maximum[0],minimum[0]);const auto greenSpan=_mm256_sub_ps(maximum[1],minimum[1]),blueSpan=_mm256_sub_ps(maximum[2],minimum[2]);
            const auto pickGreen=_mm256_cmp_ps(greenSpan,best,_CMP_GT_OQ);best=_mm256_max_ps(best,greenSpan);
            const auto pickBlue=_mm256_cmp_ps(blueSpan,best,_CMP_GT_OQ);best=_mm256_max_ps(best,blueSpan);
            if(_mm256_movemask_ps(_mm256_cmp_ps(best,_mm256_set1_ps(3),_CMP_GT_OQ))){
            const __m256i corners[8]{a,b,c,d,load(-1,-1),load(2,-1),load(-1,2),load(2,2)};
            // Select the byte before conversion. All channel values are exact
            // integers, so this is identical to converting/blending three RGB
            // vectors for each of the eight support pixels.
            const auto shift=_mm256_blendv_epi8(_mm256_blendv_epi8(_mm256_setzero_si256(),_mm256_set1_epi32(8),_mm256_castps_si256(pickGreen)),_mm256_set1_epi32(16),_mm256_castps_si256(pickBlue));
            auto selected=[&](__m256i color){return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(color,shift),_mm256_set1_epi32(255)));};
            auto lo=selected(a),hi=lo;
            for(int tap=1;tap<8;++tap){const auto v=selected(corners[tap]);lowPixel=_mm256_blendv_epi8(lowPixel,corners[tap],_mm256_castps_si256(_mm256_cmp_ps(v,lo,_CMP_LT_OQ)));highPixel=_mm256_blendv_epi8(highPixel,corners[tap],_mm256_castps_si256(_mm256_cmp_ps(v,hi,_CMP_GT_OQ)));lo=_mm256_min_ps(lo,v);hi=_mm256_max_ps(hi,v);}
            best=_mm256_sub_ps(hi,lo);
            // Below this exact threshold the confidence is zero for every lane.
            // Smooth color ramps need no RGB projection, division or residual fit.
            if(_mm256_movemask_ps(_mm256_cmp_ps(best,_mm256_set1_ps(12),_CMP_GT_OQ))){
            __m256 low[3],delta[3];auto dot=zero,energy=one;
            for(int ch=0;ch<3;++ch){low[ch]=channel(lowPixel,ch);delta[ch]=_mm256_sub_ps(channel(highPixel,ch),low[ch]);dot=_mm256_add_ps(dot,_mm256_mul_ps(_mm256_sub_ps(base[ch],low[ch]),delta[ch]));energy=_mm256_add_ps(energy,_mm256_mul_ps(delta[ch],delta[ch]));}
            const auto t=clamp(_mm256_div_ps(dot,energy));auto residual=zero;const auto absMask=_mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
            for(int ch=0;ch<3;++ch)residual=_mm256_max_ps(residual,_mm256_and_ps(absMask,_mm256_sub_ps(base[ch],_mm256_add_ps(low[ch],_mm256_mul_ps(t,delta[ch])))));
            const auto reliability=_mm256_mul_ps(clamp(_mm256_mul_ps(_mm256_sub_ps(best,_mm256_set1_ps(12)),_mm256_set1_ps(1.f/64))),clamp(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(20),residual),_mm256_set1_ps(1.f/12))));
            const auto phase=_mm256_mul_ps(_mm256_set1_ps(amount),_mm256_mul_ps(reliability,_mm256_sub_ps(_mm256_mul_ps(_mm256_mul_ps(t,t),_mm256_sub_ps(_mm256_set1_ps(3),_mm256_add_ps(t,t))),t)));
            for(int ch=0;ch<3;++ch)base[ch]=_mm256_min_ps(maximum[ch],_mm256_max_ps(minimum[ch],_mm256_add_ps(base[ch],_mm256_mul_ps(phase,delta[ch]))));
            }
            }
        }
        __m256i value[3];for(int ch=0;ch<3;++ch)value[ch]=_mm256_cvttps_epi32(_mm256_add_ps(base[ch],half));const auto packed=_mm256_or_si256(_mm256_set1_epi32(int(0xff000000u)),_mm256_or_si256(value[0],_mm256_or_si256(_mm256_slli_epi32(value[1],8),_mm256_slli_epi32(value[2],16))));if(lanes==8)_mm256_storeu_si256(reinterpret_cast<__m256i*>(target+size_t(x)*4),packed);else{alignas(32) uint8_t out[32];_mm256_store_si256(reinterpret_cast<__m256i*>(out),packed);std::memcpy(target+size_t(x)*4,out,lanes*4);}
    }
    _mm256_zeroupper();
}
}

namespace fajk {
void fastEdgeUpscaleRow(const uint8_t* source,uint32_t iw,uint32_t ih,uint8_t* target,uint32_t width,const int* low,const float* fractions,int iy,float fy,float amount){edgeUpscaleRowImpl<false,true>(source,iw,ih,target,width,low,fractions,iy,fy,amount);}
void edgeUpscaleRow(const uint8_t* source,uint32_t iw,uint32_t ih,uint8_t* target,uint32_t width,const int* low,const float* fractions,int iy,float fy,float amount,bool cubic){
    if(cubic)edgeUpscaleRowImpl<true>(source,iw,ih,target,width,low,fractions,iy,fy,amount);
    else edgeUpscaleRowImpl<false>(source,iw,ih,target,width,low,fractions,iy,fy,amount);
}
}

