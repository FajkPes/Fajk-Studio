#include <algorithm>
#include <immintrin.h>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include "color_boundary_avx2.inl"
#include "guided_chroma_avx2.inl"
#include "perceptual_color.hpp"
#include "gamma_byte_avx2.inl"
#include "recursive_cleanup_avx2.inl"

namespace fajk {
// Compute independent quantization errors together, but accumulate their
// double costs in the original coefficient order. No approximate divisions.
bool spatialRateCandidateAvx2(const float* normalized,const float* magnitudes,const float* importance,int width,int height,float factor,float rounding,int* values,float* errors,double& cost){
    const auto spacing=_mm256_set1_ps(factor),bias=_mm256_set1_ps(rounding),zero=_mm256_setzero_ps();auto occupied=_mm256_setzero_si256();double accumulated=cost;
    for(int y=0;y<height;++y)for(int x=0;x<width;x+=8){const int at=y*16+x;const auto source=_mm256_loadu_ps(normalized+at);
        auto q=_mm256_cvttps_epi32(_mm256_add_ps(_mm256_div_ps(_mm256_loadu_ps(magnitudes+at),spacing),bias));
        const auto negative=_mm256_castps_si256(_mm256_cmp_ps(source,zero,_CMP_LT_OQ));q=_mm256_sub_epi32(_mm256_xor_si256(q,negative),negative);_mm256_storeu_si256(reinterpret_cast<__m256i*>(values+at),q);occupied=_mm256_or_si256(occupied,q);
        const auto error=_mm256_sub_ps(source,_mm256_mul_ps(_mm256_cvtepi32_ps(q),spacing));_mm256_storeu_ps(errors+at,error);
        const auto weights=_mm256_loadu_ps(importance+at);alignas(32) double prices[8];
        const auto lo=_mm256_cvtps_pd(_mm256_castps256_ps128(error)),hi=_mm256_cvtps_pd(_mm256_extractf128_ps(error,1));
        _mm256_store_pd(prices,_mm256_mul_pd(_mm256_mul_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(weights)),lo),lo));
        _mm256_store_pd(prices+4,_mm256_mul_pd(_mm256_mul_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(weights,1)),hi),hi));
        for(double price:prices)accumulated+=price;
    }
    cost=accumulated;return !_mm256_testz_si256(occupied,occupied);
}
// Eight independent Gram-matrix contributions. Products retain the original
// float rounding; neighbor sums and the final row-order cost remain double.
void spatialRateCoupling8Avx2(const float* errors,int height,const float* correlations,double& cost){
    const auto right1=_mm256_castsi256_ps(_mm256_setr_epi32(-1,-1,-1,-1,-1,-1,-1,0));
    const auto right2=_mm256_castsi256_ps(_mm256_setr_epi32(-1,-1,-1,-1,-1,-1,0,0));
    const auto left1=_mm256_castsi256_ps(_mm256_setr_epi32(0,-1,-1,-1,-1,-1,-1,-1));
    const auto c0=_mm256_set1_ps(correlations[0]),c1=_mm256_set1_ps(correlations[1]),
               c2=_mm256_set1_ps(correlations[2]),c3=_mm256_set1_ps(correlations[3]),c4=_mm256_set1_ps(correlations[4]);
    for(int y=0;y<height;++y){const float* row=errors+y*16;
        auto first=_mm256_and_ps(_mm256_mul_ps(_mm256_loadu_ps(row+1),c0),right1);
        auto lo=_mm256_cvtps_pd(_mm256_castps256_ps128(first)),hi=_mm256_cvtps_pd(_mm256_extractf128_ps(first,1));
        auto add=[&](__m256 term){lo=_mm256_add_pd(lo,_mm256_cvtps_pd(_mm256_castps256_ps128(term)));hi=_mm256_add_pd(hi,_mm256_cvtps_pd(_mm256_extractf128_ps(term,1)));};
        add(_mm256_and_ps(_mm256_mul_ps(_mm256_loadu_ps(row+2),c2),right2));
        if(y+1<height){
            add(_mm256_mul_ps(_mm256_loadu_ps(row+16),c1));
            add(_mm256_and_ps(_mm256_mul_ps(_mm256_loadu_ps(row+15),c4),left1));
            add(_mm256_and_ps(_mm256_mul_ps(_mm256_loadu_ps(row+17),c4),right1));
        }
        if(y+2<height)add(_mm256_mul_ps(_mm256_loadu_ps(row+32),c3));
        const auto scaled=_mm256_mul_ps(_mm256_loadu_ps(row),_mm256_set1_ps(2));
        alignas(32) double contributions[8];
        _mm256_store_pd(contributions,_mm256_mul_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(scaled)),lo));
        _mm256_store_pd(contributions+4,_mm256_mul_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(scaled,1)),hi));
        for(double value:contributions)cost+=value;
    }
}
// Transposed synthesis uses separate additions, not the ordinary forward lift.
// Keep their order and true division so coefficients match the SSE reference.
void synthesisAdjointRowAvx2(float* data,float* scratch,int count){
    constexpr float alpha=-1.586134342f,beta=-.05298011854f,gamma=.8829110762f,delta=.4435068522f,normal=1.149604398f;
    const int half=count/2;auto* low=scratch;auto* high=low+half;
    const auto order=_mm256_setr_epi32(0,1,4,5,2,3,6,7);int i=0;
    for(;i+8<=half;i+=8){const auto a=_mm256_loadu_ps(data+i*2),b=_mm256_loadu_ps(data+i*2+8);_mm256_storeu_ps(low+i,_mm256_permutevar8x32_ps(_mm256_shuffle_ps(a,b,_MM_SHUFFLE(2,0,2,0)),order));_mm256_storeu_ps(high+i,_mm256_permutevar8x32_ps(_mm256_shuffle_ps(a,b,_MM_SHUFFLE(3,1,3,1)),order));}
    for(;i<half;++i){low[i]=data[i*2];high[i]=data[i*2+1];}
    auto add=[&](float* target,const float* source,int n,float factor){int x=0;const auto f=_mm256_set1_ps(factor);for(;x+8<=n;x+=8)_mm256_storeu_ps(target+x,_mm256_add_ps(_mm256_loadu_ps(target+x),_mm256_mul_ps(f,_mm256_loadu_ps(source+x))));for(;x<n;++x)target[x]+=factor*source[x];};
    auto transposeHigh=[&](float factor){add(low+1,high,half-1,factor);add(low,high,half,factor);low[half-1]+=factor*high[half-1];};
    auto transposeLow=[&](float factor){add(high,low,half,factor);high[0]+=factor*low[0];add(high,low+1,half-1,factor);};
    transposeHigh(-alpha);transposeLow(-beta);transposeHigh(-gamma);transposeLow(-delta);
    i=0;for(;i+8<=half;i+=8){_mm256_storeu_ps(data+i,_mm256_div_ps(_mm256_loadu_ps(low+i),_mm256_set1_ps(normal)));_mm256_storeu_ps(data+half+i,_mm256_mul_ps(_mm256_loadu_ps(high+i),_mm256_set1_ps(normal)));}
    for(;i<half;++i){data[i]=low[i]/normal;data[half+i]=high[i]*normal;}
}
void synthesisAdjointColumnsAvx2(float* data,float* scratch,int stride,int rows,int begin,int end){
    constexpr float alpha=-1.586134342f,beta=-.05298011854f,gamma=.8829110762f,delta=.4435068522f,normal=1.149604398f;
    const int width=end-begin,half=rows/2;
    for(int y=0;y<rows;++y){const int from=y<half?2*y:2*(y-half)+1;std::copy_n(data+size_t(from)*stride+begin,width,scratch+size_t(y)*width);}
    auto add=[&](int target,int source,float factor){auto* out=scratch+size_t(target)*width;const auto* in=scratch+size_t(source)*width;const auto f=_mm256_set1_ps(factor);int x=0;for(;x+8<=width;x+=8)_mm256_storeu_ps(out+x,_mm256_add_ps(_mm256_loadu_ps(out+x),_mm256_mul_ps(f,_mm256_loadu_ps(in+x))));for(;x<width;++x)out[x]+=factor*in[x];};
    auto high=[&](float factor){for(int y=1;y<half;++y)add(y,half+y-1,factor);for(int y=0;y<half;++y)add(y,half+y,factor);add(half-1,rows-1,factor);};
    auto low=[&](float factor){for(int y=0;y<half;++y)add(half+y,y,factor);add(half,0,factor);for(int y=0;y+1<half;++y)add(half+y,y+1,factor);};
    high(-alpha);low(-beta);high(-gamma);low(-delta);
    for(int y=0;y<rows;++y){const auto* in=scratch+size_t(y)*width;auto* out=data+size_t(y)*stride+begin;int x=0;for(;x+8<=width;x+=8){const auto v=_mm256_loadu_ps(in+x);_mm256_storeu_ps(out+x,y<half?_mm256_div_ps(v,_mm256_set1_ps(normal)):_mm256_mul_ps(v,_mm256_set1_ps(normal)));}for(;x<width;++x)out[x]=y<half?in[x]/normal:in[x]*normal;}
}
uint32_t resampleLinearAvx2(const uint8_t* top,const uint8_t* bottom,const int* low,const int* high,const float* left,const float* right,float above,float below,uint32_t width,uint8_t* output){
    const auto mask=_mm256_set1_epi32(255),alpha=_mm256_set1_epi32(int(0xff000000u));const auto up=_mm256_set1_ps(above),down=_mm256_set1_ps(below),half=_mm256_set1_ps(.5f);
    const int lastInput=width?std::max(low[width-1],high[width-1]):-1;
    uint32_t x=0;for(;x+8<=width;x+=8){auto a=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(low+x)),b=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(high+x));
        // Upscaling addresses repeat nearby pixels. Load contiguous RGBA once
        // and permute it instead of issuing eight independent gathers per row.
        auto fetch=[&](const uint8_t* row,const int* indices,__m256i positions){
            const int first=indices[x];
            if(first+7<=lastInput&&indices[x+7]-first<=7){const auto packed=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(row+size_t(first)*4));return _mm256_permutevar8x32_epi32(packed,_mm256_sub_epi32(positions,_mm256_set1_epi32(first)));}
            return _mm256_i32gather_epi32(reinterpret_cast<const int*>(row),positions,4);
        };
        const auto tl=fetch(top,low,a),tr=fetch(top,high,b),bl=fetch(bottom,low,a),br=fetch(bottom,high,b);
        const auto wl=_mm256_loadu_ps(left+x),wr=_mm256_loadu_ps(right+x);
        auto channel=[&](int c){auto convert=[&](__m256i v){if(c==1)v=_mm256_srli_epi32(v,8);if(c==2)v=_mm256_srli_epi32(v,16);return _mm256_cvtepi32_ps(_mm256_and_si256(v,mask));};
            auto upper=_mm256_add_ps(_mm256_mul_ps(convert(tl),wl),_mm256_mul_ps(convert(tr),wr));auto lower=_mm256_add_ps(_mm256_mul_ps(convert(bl),wl),_mm256_mul_ps(convert(br),wr));return _mm256_cvttps_epi32(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(upper,up),_mm256_mul_ps(lower,down)),half));};
        const auto rgb=_mm256_or_si256(channel(0),_mm256_or_si256(_mm256_slli_epi32(channel(1),8),_mm256_slli_epi32(channel(2),16)));_mm256_storeu_si256(reinterpret_cast<__m256i*>(output+size_t(x)*4),_mm256_or_si256(rgb,alpha));
    }return x;
}
uint32_t r12RgbCompactRowAvx2(const uint8_t* previous,const uint8_t* current,const uint8_t* next,uint8_t* output,uint32_t x,uint32_t width,float threshold,float blend){
    const auto mask=_mm256_set1_epi32(255),alpha=_mm256_set1_epi32(int(0xff000000u));
    const auto sign=_mm256_set1_ps(-0.f),one=_mm256_set1_ps(1.f),inverse=_mm256_set1_ps(1.f/threshold),four=_mm256_set1_ps(4.f);
    auto channels=[&](__m256i rgb,__m256 (&v)[3]){v[0]=_mm256_cvtepi32_ps(_mm256_and_si256(rgb,mask));v[1]=_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(rgb,8),mask));v[2]=_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(rgb,16),mask));};
    for(;x+9<=width;x+=8){__m256 center[3],sum[3];const auto packed=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(current+size_t(x)*4));channels(packed,center);for(int c=0;c<3;++c)sum[c]=_mm256_mul_ps(center[c],four);auto total=four;
        const uint8_t* taps[8]={current+(size_t(x)+1)*4,current+(size_t(x)-1)*4,next+size_t(x)*4,previous+size_t(x)*4,next+(size_t(x)+1)*4,next+(size_t(x)-1)*4,previous+(size_t(x)+1)*4,previous+(size_t(x)-1)*4};
        for(int t=0;t<8;++t){__m256 neighbor[3];const auto other=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(taps[t]));channels(other,neighbor);
            // RGB bytes have integral differences: measure the maximum in
            // packed byte lanes, then convert once for the unchanged gate.
            const auto delta=_mm256_and_si256(_mm256_or_si256(_mm256_subs_epu8(other,packed),_mm256_subs_epu8(packed,other)),_mm256_set1_epi32(0x00ffffff));
            const auto maximum=_mm256_max_epu8(_mm256_max_epu8(delta,_mm256_srli_epi32(delta,8)),_mm256_srli_epi32(delta,16));
            const auto difference=_mm256_cvtepi32_ps(_mm256_and_si256(maximum,mask));auto gate=_mm256_max_ps(_mm256_setzero_ps(),_mm256_sub_ps(one,_mm256_mul_ps(difference,inverse)));auto weight=_mm256_mul_ps(_mm256_mul_ps(gate,gate),_mm256_set1_ps(t<4?1.f:7*.1f));total=_mm256_add_ps(total,weight);for(int c=0;c<3;++c)sum[c]=_mm256_add_ps(sum[c],_mm256_mul_ps(neighbor[c],weight));}
        __m256i result[3];for(int c=0;c<3;++c){auto average=_mm256_div_ps(sum[c],total);if(blend<1)average=_mm256_add_ps(center[c],_mm256_mul_ps(_mm256_sub_ps(average,center[c]),_mm256_set1_ps(blend)));result[c]=_mm256_cvttps_epi32(_mm256_add_ps(average,_mm256_set1_ps(.5f)));}
        auto rgba=_mm256_or_si256(result[0],_mm256_or_si256(_mm256_slli_epi32(result[1],8),_mm256_slli_epi32(result[2],16)));rgba=_mm256_or_si256(rgba,_mm256_and_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(current+size_t(x)*4)),alpha));_mm256_storeu_si256(reinterpret_cast<__m256i*>(output+size_t(x)*4),rgba);
    }return x;
}
// The operation order matches the historical SSE predictor. Eight pixels
// share one model segment; no interpolation lookup in the inner loop.
uint32_t r6CorrectionSpanAvx2(const float* luma,float* cb,float* cr,uint32_t count,const float* initial,const float* delta){
    const auto offsets=_mm256_setr_ps(0,1,2,3,4,5,6,7);
    uint32_t x=0;for(;x+8<=count;x+=8){
        const auto positions=_mm256_add_ps(_mm256_set1_ps(float(x)),offsets);
        const auto mean=_mm256_add_ps(_mm256_set1_ps(initial[2]),_mm256_mul_ps(positions,_mm256_set1_ps(delta[2])));
        const auto ac=_mm256_sub_ps(_mm256_loadu_ps(luma+x),mean);
        _mm256_storeu_ps(cb+x,_mm256_mul_ps(_mm256_add_ps(_mm256_set1_ps(initial[0]),_mm256_mul_ps(positions,_mm256_set1_ps(delta[0]))),ac));
        _mm256_storeu_ps(cr+x,_mm256_mul_ps(_mm256_add_ps(_mm256_set1_ps(initial[1]),_mm256_mul_ps(positions,_mm256_set1_ps(delta[1]))),ac));
    }return x;
}
void resampleHorizontalPairsAvx2(const uint8_t* input,float* converted,uint32_t inputWidth,const int* indices,const float* weights,size_t taps,uint32_t width,float* output){
    uint32_t x=0;
    for(;x+2<=inputWidth;x+=2){const auto rgba=_mm_loadl_epi64(reinterpret_cast<const __m128i*>(input+size_t(x)*4));_mm256_storeu_ps(converted+size_t(x)*4,_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(rgba)));}
    if(x<inputWidth){int packed;std::memcpy(&packed,input+size_t(x)*4,4);const auto rgba=_mm_cvtsi32_si128(packed);_mm_storeu_ps(converted+size_t(x)*4,_mm_cvtepi32_ps(_mm_cvtepu8_epi32(rgba)));}
    x=0;
    for(;x+2<=width;x+=2){auto value=_mm256_setzero_ps();for(size_t j=0;j<taps;++j){const size_t a=size_t(x)*taps+j,b=a+taps;
        const auto colors=_mm256_set_m128(_mm_loadu_ps(converted+size_t(indices[b])*4),_mm_loadu_ps(converted+size_t(indices[a])*4));
        const auto gain=_mm256_set_m128(_mm_set1_ps(weights[b]),_mm_set1_ps(weights[a]));value=_mm256_add_ps(value,_mm256_mul_ps(colors,gain));
    }_mm256_storeu_ps(output+size_t(x)*4,value);}
    if(x<width){auto value=_mm_setzero_ps();for(size_t j=0;j<taps;++j){const size_t at=size_t(x)*taps+j;value=_mm_add_ps(value,_mm_mul_ps(_mm_loadu_ps(converted+size_t(indices[at])*4),_mm_set1_ps(weights[at])));}_mm_storeu_ps(output+size_t(x)*4,value);}
}
uint32_t resampleVerticalAvx2(const float* const* rows,const float* weights,size_t taps,uint32_t width,uint8_t* output){
    const auto zero=_mm256_setzero_ps(),limit=_mm256_set1_ps(255.f),half=_mm256_set1_ps(.5f);
    const auto order=_mm256_setr_epi32(0,4,1,5,2,6,3,7),alpha=_mm256_set1_epi32(int(0xff000000u));
    uint32_t x=0;for(;x+8<=width;x+=8){auto a=zero,b=zero,c=zero,d=zero;
        for(size_t j=0;j<taps;++j){const auto* row=rows[j]+size_t(x)*4;const auto weight=_mm256_set1_ps(weights[j]);
            a=_mm256_add_ps(a,_mm256_mul_ps(_mm256_loadu_ps(row),weight));b=_mm256_add_ps(b,_mm256_mul_ps(_mm256_loadu_ps(row+8),weight));
            c=_mm256_add_ps(c,_mm256_mul_ps(_mm256_loadu_ps(row+16),weight));d=_mm256_add_ps(d,_mm256_mul_ps(_mm256_loadu_ps(row+24),weight));}
        auto integer=[&](__m256 value){return _mm256_cvttps_epi32(_mm256_add_ps(_mm256_min_ps(limit,_mm256_max_ps(zero,value)),half));};
        auto packed=_mm256_packus_epi16(_mm256_packs_epi32(integer(a),integer(b)),_mm256_packs_epi32(integer(c),integer(d)));
        packed=_mm256_or_si256(_mm256_permutevar8x32_epi32(packed,order),alpha);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(output+size_t(x)*4),packed);
    }return x;
}
bool waveletOpaqueAvx2(const uint8_t* pixels,size_t bytes){
    const auto mask=_mm256_set1_epi32(int(0xff000000u));size_t i=0;
    for(;i+128<=bytes;i+=128){auto v=_mm256_and_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(pixels+i)),_mm256_loadu_si256(reinterpret_cast<const __m256i*>(pixels+i+32)));
        auto w=_mm256_and_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(pixels+i+64)),_mm256_loadu_si256(reinterpret_cast<const __m256i*>(pixels+i+96)));
        if(!_mm256_testc_si256(_mm256_and_si256(v,w),mask))return false;}
    for(;i<bytes;i+=4)if(pixels[i+3]!=255)return false;return true;
}
void waveletHistogramAvx2(const float* samples,int width,float inverse,uint32_t rows,uint32_t* histogram){
    const auto factor=_mm256_set1_ps(inverse*4),sign=_mm256_set1_ps(-0.f);const auto limit=_mm256_set1_epi32(8191),offsets=_mm256_setr_epi32(0,4,8,12,16,20,24,28);int x=0;
    for(;x+32<=width;x+=32){const auto values=_mm256_andnot_ps(sign,_mm256_i32gather_ps(samples+x,offsets,4));
        const auto bins=_mm256_min_epi32(limit,_mm256_cvttps_epi32(_mm256_mul_ps(values,factor)));
        if(_mm256_testz_si256(bins,bins))continue;
        alignas(32) int indices[8];_mm256_store_si256(reinterpret_cast<__m256i*>(indices),bins);
        for(int i=0;i<8;++i)if(indices[i])histogram[indices[i]]+=rows*4;
    }
    for(;x<width;x+=4){const int bin=std::min(8191,int(std::abs(samples[x])*inverse*4));if(bin)histogram[bin]+=rows*std::min(4,width-x);}
}
void waveletHistogramDenseAvx2(const float* samples,int width,float inverse,uint32_t* histogram){
    const auto factor=_mm256_set1_ps(inverse*4),sign=_mm256_set1_ps(-0.f);const auto limit=_mm256_set1_epi32(8191);int x=0;
    for(;x+8<=width;x+=8){const auto values=_mm256_andnot_ps(sign,_mm256_loadu_ps(samples+x));
        const auto bins=_mm256_min_epi32(limit,_mm256_cvttps_epi32(_mm256_mul_ps(values,factor)));
        if(_mm256_testz_si256(bins,bins))continue;
        alignas(32) int indices[8];_mm256_store_si256(reinterpret_cast<__m256i*>(indices),bins);
        for(int i=0;i<8;++i)if(indices[i])++histogram[indices[i]];
    }
    for(;x<width;++x){const int bin=std::min(8191,int(std::abs(samples[x])*inverse*4));if(bin)++histogram[bin];}
}
bool waveletQuantize16Avx2(const float* samples,float inverse,int* output,float rounding){
    const auto scale=_mm256_set1_ps(inverse),sign=_mm256_set1_ps(-0.f),bias=_mm256_set1_ps(rounding);
    const auto a=_mm256_mul_ps(_mm256_loadu_ps(samples),scale),b=_mm256_mul_ps(_mm256_loadu_ps(samples+8),scale);
    const auto ma=_mm256_cvttps_epi32(_mm256_add_ps(_mm256_andnot_ps(sign,a),bias)),mb=_mm256_cvttps_epi32(_mm256_add_ps(_mm256_andnot_ps(sign,b),bias));
    const auto either=_mm256_or_si256(ma,mb);if(_mm256_testz_si256(either,either))return false;
    const auto sa=_mm256_srai_epi32(_mm256_castps_si256(a),31),sb=_mm256_srai_epi32(_mm256_castps_si256(b),31);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(output),_mm256_sub_epi32(_mm256_xor_si256(ma,sa),sa));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(output+8),_mm256_sub_epi32(_mm256_xor_si256(mb,sb),sb));return true;
}
int nativeRestoreDirectionAvx2(const float* source,int stride,int top,int bottom,int left){
    const int offsets[4]={1,stride,stride+1,stride-1};float costs[4];
    // Pair directions in vector halves. Each half retains the reference's
    // four-lane accumulation order, including its second group of four pixels.
    for(int d=0;d<4;d+=2){auto energy=_mm256_setzero_ps();
        for(int y=top;y<bottom;y+=2)for(int x=left;x<left+8;x+=4){const auto* at=source+size_t(y)*stride+x;
            const auto a=_mm_sub_ps(_mm_loadu_ps(at+offsets[d]),_mm_loadu_ps(at-offsets[d]));
            const auto b=_mm_sub_ps(_mm_loadu_ps(at+offsets[d+1]),_mm_loadu_ps(at-offsets[d+1]));
            const auto delta=_mm256_set_m128(b,a);energy=_mm256_add_ps(energy,_mm256_mul_ps(delta,delta));
        }
        alignas(32) float lanes[8];_mm256_store_ps(lanes,energy);
        for(int c=0;c<2;++c){const auto* p=lanes+c*4;costs[d+c]=(p[0]+p[1])+(p[2]+p[3]);}
    }
    int direction=0;for(int d=1;d<4;++d)if(costs[d]<costs[direction])direction=d;return direction;
}
int nativeRestoreBlockRowAvx2(const float* source,float* target,int begin,int end,int primary,int secondary,float strength){
    const auto zero=_mm256_setzero_ps(),sign=_mm256_set1_ps(-0.f),quarter=_mm256_set1_ps(.25f);
    auto constrain=[&](__m256 delta,float limit){const auto magnitude=_mm256_andnot_ps(sign,delta);const auto allowed=_mm256_max_ps(zero,_mm256_sub_ps(_mm256_set1_ps(limit),_mm256_mul_ps(magnitude,quarter)));return _mm256_or_ps(_mm256_min_ps(magnitude,allowed),_mm256_and_ps(sign,delta));};
    int x=begin;for(;x+8<=end;x+=8){const auto center=_mm256_loadu_ps(source+x);auto sum=zero;
        for(int side:{-1,1}){
            sum=_mm256_add_ps(sum,_mm256_mul_ps(constrain(_mm256_sub_ps(_mm256_loadu_ps(source+x+side*primary),center),strength),_mm256_set1_ps(3.f/16)));
            sum=_mm256_add_ps(sum,_mm256_mul_ps(constrain(_mm256_sub_ps(_mm256_loadu_ps(source+x+side*2*primary),center),strength),_mm256_set1_ps(2.f/16)));
            sum=_mm256_add_ps(sum,_mm256_mul_ps(constrain(_mm256_sub_ps(_mm256_loadu_ps(source+x+side*secondary),center),strength*.5f),_mm256_set1_ps(1.f/16)));
        }_mm256_storeu_ps(target+x,_mm256_add_ps(center,sum));
    }return x;
}
void nativeRestoreBlockAvx2(const float* source,float* target,int stride,int top,int bottom,int left,int primary,int secondary,float strength){
    const auto zero=_mm256_setzero_ps(),sign=_mm256_set1_ps(-0.f),quarter=_mm256_set1_ps(.25f),full=_mm256_set1_ps(strength),half=_mm256_set1_ps(strength*.5f);
    const auto w0=_mm256_set1_ps(3.f/16),w1=_mm256_set1_ps(2.f/16),w2=_mm256_set1_ps(1.f/16);
    auto constrain=[&](__m256 delta,__m256 limit){const auto magnitude=_mm256_andnot_ps(sign,delta),allowed=_mm256_max_ps(zero,_mm256_sub_ps(limit,_mm256_mul_ps(magnitude,quarter)));return _mm256_or_ps(_mm256_min_ps(magnitude,allowed),_mm256_and_ps(sign,delta));};
    for(int y=top;y<bottom;++y){const auto* row=source+size_t(y)*stride+left;const auto center=_mm256_loadu_ps(row);auto sum=zero;for(int side:{-1,1}){
        sum=_mm256_add_ps(sum,_mm256_mul_ps(constrain(_mm256_sub_ps(_mm256_loadu_ps(row+side*primary),center),full),w0));
        sum=_mm256_add_ps(sum,_mm256_mul_ps(constrain(_mm256_sub_ps(_mm256_loadu_ps(row+side*2*primary),center),full),w1));
        sum=_mm256_add_ps(sum,_mm256_mul_ps(constrain(_mm256_sub_ps(_mm256_loadu_ps(row+side*secondary),center),half),w2));
    }_mm256_storeu_ps(target+size_t(y)*stride+left,_mm256_add_ps(center,sum));}
}
static uint32_t waveletColorRowAvx2Impl(const float* luma,const float* cb,const float* cr,int cw,int ch,uint32_t width,uint32_t y,bool chroma420,uint32_t smoothing,uint8_t* rgba,const float* upper,const float* lower,float sharp,const uint8_t* predictor,float deringing,const uint8_t* flatRow,const uint32_t* flatColors,uint32_t flatTile,const float* cflCb,const float* cflCr,const float* gammaTable,uint8_t proxy=0){
    const float neighbor=.25f*smoothing/100.f,center=1-neighbor;
    const auto c=_mm256_set1_ps(center),n=_mm256_set1_ps(neighbor);
    auto chroma=[&](const float* plane,uint32_t x){if(!chroma420)return _mm256_loadu_ps(plane+size_t(y)*cw+x);
        const int cx=int(x/2),cy=int(y/2);const float* a=plane+size_t(cy)*cw;
        if(!smoothing){const auto values=_mm256_broadcast_ps(reinterpret_cast<const __m128*>(a+cx));return _mm256_permutevar8x32_ps(values,_mm256_setr_epi32(0,0,1,1,2,2,3,3));}
        const int begin=std::max(0,cx-1),ny=std::clamp(cy+(y%2?1:-1),0,ch-1);
        auto load=[&](const float* row){if(begin+8<=cw)return _mm256_loadu_ps(row+begin);
            alignas(32) float values[8];for(int i=0;i<8;++i)values[i]=row[std::min(cw-1,begin+i)];return _mm256_load_ps(values);};
        const auto values=_mm256_add_ps(_mm256_mul_ps(load(a),c),_mm256_mul_ps(load(plane+size_t(ny)*cw),n));
        const auto centers=cx?_mm256_setr_epi32(1,1,2,2,3,3,4,4):_mm256_setr_epi32(0,0,1,1,2,2,3,3);
        const auto neighbors=cx?_mm256_setr_epi32(0,2,1,3,2,4,3,5):_mm256_setr_epi32(0,1,0,2,1,3,2,4);
        return _mm256_add_ps(_mm256_mul_ps(_mm256_permutevar8x32_ps(values,centers),c),_mm256_mul_ps(_mm256_permutevar8x32_ps(values,neighbors),n));
    };
    const auto zero=_mm256_setzero_ps(),limit=_mm256_set1_ps(255),half=_mm256_set1_ps(.5f);
    auto toInt=[&](__m256 value){return _mm256_cvttps_epi32(_mm256_add_ps(half,_mm256_min_ps(limit,_mm256_max_ps(zero,value))));};
    const auto* gammaBytes=gammaTable?perceptualColor::gammaExponentByteTable().data():nullptr;
    auto gamma=[&](__m256 linear){return gammaByteVectorAvx2(linear,gammaTable,gammaBytes);};
    auto combine=[](__m256 a,float ca,__m256 b,float cb,__m256 c,float cc){return _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(a,_mm256_set1_ps(ca)),_mm256_mul_ps(b,_mm256_set1_ps(cb))),_mm256_mul_ps(c,_mm256_set1_ps(cc)));};
    uint32_t x=0;for(;x+8<=width;x+=8){
        if(flatRow&&flatRow[x/flatTile]){_mm256_storeu_si256(reinterpret_cast<__m256i*>(rgba+x*4),_mm256_set1_epi32(int(flatColors[flatRow[x/flatTile]])));continue;}
        auto light=_mm256_loadu_ps(luma+x);
        if(sharp||deringing){const auto up=_mm256_loadu_ps(upper+x),down=_mm256_loadu_ps(lower+x);
            auto adjacent=[&](int dx){if((dx>0&&x+8<width)||(dx<0&&x)){return _mm256_loadu_ps(luma+x+dx);}alignas(32) float values[8];for(int i=0;i<8;++i)values[i]=luma[std::clamp(int(x)+i+dx,0,int(width)-1)];return _mm256_load_ps(values);};
            const auto left=adjacent(-1),right=adjacent(1);
            const auto average=_mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(up,down),_mm256_add_ps(left,right)),_mm256_set1_ps(.25f));
            const auto minimum=_mm256_min_ps(light,_mm256_min_ps(_mm256_min_ps(up,down),_mm256_min_ps(left,right))),maximum=_mm256_max_ps(light,_mm256_max_ps(_mm256_max_ps(up,down),_mm256_max_ps(left,right)));
            if(deringing){const auto sign=_mm256_set1_ps(-0.f),threshold=_mm256_set1_ps(deringing);auto contribution=[&](__m256 value){const auto d=_mm256_sub_ps(value,light);return _mm256_and_ps(d,_mm256_cmp_ps(_mm256_andnot_ps(sign,d),threshold,_CMP_LT_OQ));};
                const auto correction=_mm256_add_ps(_mm256_add_ps(contribution(up),contribution(down)),_mm256_add_ps(contribution(left),contribution(right)));light=_mm256_add_ps(light,_mm256_mul_ps(correction,_mm256_set1_ps(.125f)));}
            light=_mm256_min_ps(maximum,_mm256_max_ps(minimum,_mm256_add_ps(light,_mm256_mul_ps(_mm256_sub_ps(light,average),_mm256_set1_ps(sharp)))));
        }
        const auto yy=_mm256_add_ps(light,_mm256_set1_ps(128));auto co=chroma(cb,x),cg=chroma(cr,x);if(cflCb){const auto source=_mm256_loadu_ps(luma+x);co=_mm256_add_ps(co,_mm256_loadu_ps(cflCb+x));cg=_mm256_add_ps(cg,_mm256_loadu_ps(cflCr+x));}
        __m256i r,g,b;
        if(proxy){
            __m256 components[3];const int axis=proxy-3;components[axis]=yy;
            components[(axis+1)%3]=_mm256_add_ps(yy,_mm256_add_ps(co,co));
            components[(axis+2)%3]=_mm256_add_ps(yy,_mm256_add_ps(cg,cg));
            r=toInt(components[0]);g=toInt(components[1]);b=toInt(components[2]);
        }else if(gammaTable){
            const auto L=_mm256_mul_ps(yy,_mm256_set1_ps(1.f/255)),a=_mm256_mul_ps(co,_mm256_set1_ps(1.f/384)),bb=_mm256_mul_ps(cg,_mm256_set1_ps(1.f/384));
            auto l=combine(L,1,a,.3963377774f,bb,.2158037573f),m=combine(L,1,a,-.1055613458f,bb,-.0638541728f),s=combine(L,1,a,-.0894841775f,bb,-1.2914855480f);
            l=_mm256_mul_ps(l,_mm256_mul_ps(l,l));m=_mm256_mul_ps(m,_mm256_mul_ps(m,m));s=_mm256_mul_ps(s,_mm256_mul_ps(s,s));
            r=gamma(combine(l,4.0767416621f,m,-3.3077115913f,s,.2309699292f));
            g=gamma(combine(l,-1.2684380046f,m,2.6097574011f,s,-.3413193965f));
            b=gamma(combine(l,-.0041960863f,m,-.7034186147f,s,1.7076147010f));
        }else{
            r=toInt(_mm256_add_ps(yy,_mm256_mul_ps(cg,_mm256_set1_ps(1.402f))));
            g=toInt(_mm256_sub_ps(_mm256_sub_ps(yy,_mm256_mul_ps(co,_mm256_set1_ps(.344136f))),_mm256_mul_ps(cg,_mm256_set1_ps(.714136f))));
            b=toInt(_mm256_add_ps(yy,_mm256_mul_ps(co,_mm256_set1_ps(1.772f))));
        }
        auto pixels=_mm256_or_si256(_mm256_or_si256(r,_mm256_slli_epi32(g,8)),_mm256_or_si256(_mm256_slli_epi32(b,16),_mm256_set1_epi32(int(0xff000000u))));
        if(predictor){const auto original=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(predictor+x*4)),z=_mm256_setzero_si256(),offset=_mm256_set1_epi16(256);auto lo=_mm256_sub_epi16(_mm256_add_epi16(_mm256_unpacklo_epi8(original,z),_mm256_slli_epi16(_mm256_unpacklo_epi8(pixels,z),1)),offset),hi=_mm256_sub_epi16(_mm256_add_epi16(_mm256_unpackhi_epi8(original,z),_mm256_slli_epi16(_mm256_unpackhi_epi8(pixels,z),1)),offset);pixels=_mm256_or_si256(_mm256_packus_epi16(lo,hi),_mm256_set1_epi32(int(0xff000000u)));}
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(rgba+x*4),pixels);
    }
    return x;
}
uint32_t waveletColorRowAvx2(const float* luma,const float* cb,const float* cr,int cw,int ch,uint32_t width,uint32_t y,bool chroma420,uint32_t smoothing,uint8_t* rgba,const float* upper,const float* lower,float sharp,const uint8_t* predictor,float deringing,const uint8_t* flatRow,const uint32_t* flatColors,uint32_t flatTile,const float* cflCb,const float* cflCr){
    return waveletColorRowAvx2Impl(luma,cb,cr,cw,ch,width,y,chroma420,smoothing,rgba,upper,lower,sharp,predictor,deringing,flatRow,flatColors,flatTile,cflCb,cflCr,nullptr);
}
uint32_t waveletProxyColorRowAvx2(const float* luma,const float* cb,const float* cr,int cw,int ch,uint32_t width,uint32_t y,bool chroma420,uint32_t smoothing,uint8_t* rgba,const float* cflCb,const float* cflCr,uint8_t proxy){
    return waveletColorRowAvx2Impl(luma,cb,cr,cw,ch,width,y,chroma420,smoothing,rgba,nullptr,nullptr,0,nullptr,0,nullptr,nullptr,16,cflCb,cflCr,nullptr,proxy);
}
uint32_t waveletOklabColorRowAvx2(const float* luma,const float* cb,const float* cr,int cw,int ch,uint32_t width,uint32_t y,bool chroma420,uint32_t smoothing,uint8_t* rgba,const float* cflCb,const float* cflCr,const float* gammaTable){
    return waveletColorRowAvx2Impl(luma,cb,cr,cw,ch,width,y,chroma420,smoothing,rgba,nullptr,nullptr,0,nullptr,0,nullptr,nullptr,16,cflCb,cflCr,gammaTable);
}
uint32_t waveletColorRowAvx2(const float* luma,const float* cb,const float* cr,int cw,int ch,uint32_t width,uint32_t y,bool chroma420,uint32_t smoothing,uint8_t* rgba,const float* upper,const float* lower,float sharp,const uint8_t* predictor,float deringing,const uint8_t* flatRow,const uint32_t* flatColors,uint32_t flatTile){
    return waveletColorRowAvx2(luma,cb,cr,cw,ch,width,y,chroma420,smoothing,rgba,upper,lower,sharp,predictor,deringing,flatRow,flatColors,flatTile,nullptr,nullptr);
}
void waveletRowAvx2(float* samples,float* scratch,int count,bool inverse){
    constexpr float alpha=-1.586134342f,beta=-.05298011854f,gamma=.8829110762f,delta=.4435068522f,normal=1.149604398f;
    const int half=count/2;float* lo=scratch;float* hi=scratch+half;
    auto lift=[&](float* dst,const float* neighbor,float factor,bool next){const auto scale=_mm256_set1_ps(factor);int i=0;
        if(!next){dst[0]+=factor*(neighbor[0]+neighbor[0]);i=1;}
        for(;i+8<(next?half:half+1);i+=8)_mm256_storeu_ps(dst+i,_mm256_add_ps(_mm256_loadu_ps(dst+i),_mm256_mul_ps(scale,_mm256_add_ps(_mm256_loadu_ps(neighbor+i),_mm256_loadu_ps(neighbor+i+(next?1:-1))))));
        for(;i<half;++i)dst[i]+=factor*(neighbor[i]+neighbor[std::clamp(i+(next?1:-1),0,half-1)]);
    };
    if(inverse){int i=0;
        for(;i+8<=half;i+=8){_mm256_storeu_ps(lo+i,_mm256_mul_ps(_mm256_loadu_ps(samples+i),_mm256_set1_ps(1/normal)));_mm256_storeu_ps(hi+i,_mm256_mul_ps(_mm256_loadu_ps(samples+half+i),_mm256_set1_ps(normal)));}
        for(;i<half;++i){lo[i]=samples[i]/normal;hi[i]=samples[half+i]*normal;}
        lift(lo,hi,-delta,false);lift(hi,lo,-gamma,true);lift(lo,hi,-beta,false);lift(hi,lo,-alpha,true);
        i=0;for(;i+8<=half;i+=8){const auto a=_mm256_loadu_ps(lo+i),b=_mm256_loadu_ps(hi+i),lower=_mm256_unpacklo_ps(a,b),upper=_mm256_unpackhi_ps(a,b);_mm256_storeu_ps(samples+i*2,_mm256_permute2f128_ps(lower,upper,0x20));_mm256_storeu_ps(samples+i*2+8,_mm256_permute2f128_ps(lower,upper,0x31));}
        for(;i<half;++i){samples[i*2]=lo[i];samples[i*2+1]=hi[i];}
    }else{int i=0;const auto order=_mm256_setr_epi32(0,1,4,5,2,3,6,7);
        for(;i+8<=half;i+=8){const auto a=_mm256_loadu_ps(samples+i*2),b=_mm256_loadu_ps(samples+i*2+8);_mm256_storeu_ps(lo+i,_mm256_permutevar8x32_ps(_mm256_shuffle_ps(a,b,_MM_SHUFFLE(2,0,2,0)),order));_mm256_storeu_ps(hi+i,_mm256_permutevar8x32_ps(_mm256_shuffle_ps(a,b,_MM_SHUFFLE(3,1,3,1)),order));}
        for(;i<half;++i){lo[i]=samples[i*2];hi[i]=samples[i*2+1];}
        lift(hi,lo,alpha,true);lift(lo,hi,beta,false);lift(hi,lo,gamma,true);lift(lo,hi,delta,false);
        i=0;for(;i+8<=half;i+=8){_mm256_storeu_ps(samples+i,_mm256_mul_ps(_mm256_loadu_ps(lo+i),_mm256_set1_ps(normal)));_mm256_storeu_ps(samples+half+i,_mm256_mul_ps(_mm256_loadu_ps(hi+i),_mm256_set1_ps(1/normal)));}
        for(;i<half;++i){samples[i]=lo[i]*normal;samples[half+i]=hi[i]/normal;}
    }
}
// This translation unit is entered only after the CPU and OS AVX2 check.
void waveletColumnRangeAvx2(float* image,float* scratch,int stride,int w,int h,int begin,int end,bool inverse){
    constexpr float alpha=-1.586134342f,beta=-.05298011854f,gamma=.8829110762f,delta=.4435068522f,normal=1.149604398f;
    const int half=h/2;
    // Each worker owns a contiguous strip. Repeated lifting passes then stay
    // in its cache instead of revisiting one small slice of every image row.
    scratch+=size_t(begin)*h;w=end-begin;
    for(int y=0;y<h;++y){const int source=inverse?y:(y<half?y*2:(y-half)*2+1);
        const auto* src=image+size_t(source)*stride+begin;auto* dst=scratch+size_t(y)*w;
        if(inverse){const auto factor=_mm256_set1_ps(y<half?1/normal:normal);int x=0;
            for(;x+8<=w;x+=8)_mm256_storeu_ps(dst+x,_mm256_mul_ps(_mm256_loadu_ps(src+x),factor));
            for(;x<w;++x)dst[x]=src[x]*(y<half?1/normal:normal);
        }else std::copy_n(src,w,dst);}
    auto lift=[&](bool low,float factor){const int target=low?0:half,other=low?half:0;const auto scale=_mm256_set1_ps(factor);
        for(int y=0;y<half;++y){auto* dst=scratch+size_t(target+y)*w;const auto* a=scratch+size_t(other+y)*w;
            const auto* b=scratch+size_t(other+std::clamp(y+(low?-1:1),0,half-1))*w;int x=0;
            for(;x+8<=w;x+=8)_mm256_storeu_ps(dst+x,_mm256_add_ps(_mm256_loadu_ps(dst+x),_mm256_mul_ps(scale,_mm256_add_ps(_mm256_loadu_ps(a+x),_mm256_loadu_ps(b+x)))));
            for(;x<w;++x)dst[x]+=factor*(a[x]+b[x]);
        }
    };
    if(inverse){
        lift(true,-delta);lift(false,-gamma);
        // Finish the two dependent lifts together. Keep the current/next low
        // sample in registers and write interleaved output directly, instead
        // of storing and rereading both completed bands in scratch.
        const auto b=_mm256_set1_ps(-beta),a=_mm256_set1_ps(-alpha);
        int x=0;
        for(;x+8<=w;x+=8){
            auto high=_mm256_loadu_ps(scratch+size_t(half)*w+x);
            auto low=_mm256_add_ps(_mm256_loadu_ps(scratch+x),_mm256_mul_ps(b,_mm256_add_ps(high,high)));
            for(int y=0;y<half;++y){
                auto nextLow=low,nextHigh=high;
                if(y+1<half){
                    nextHigh=_mm256_loadu_ps(scratch+size_t(half+y+1)*w+x);
                    nextLow=_mm256_add_ps(_mm256_loadu_ps(scratch+size_t(y+1)*w+x),
                        _mm256_mul_ps(b,_mm256_add_ps(nextHigh,high)));
                }
                const auto finalHigh=_mm256_add_ps(high,_mm256_mul_ps(a,_mm256_add_ps(low,nextLow)));
                _mm256_storeu_ps(image+size_t(y*2)*stride+begin+x,low);
                _mm256_storeu_ps(image+size_t(y*2+1)*stride+begin+x,finalHigh);
                low=nextLow;high=nextHigh;
            }
        }
        for(;x<w;++x){
            float high=scratch[size_t(half)*w+x],low=scratch[x]+(-beta)*(high+high);
            for(int y=0;y<half;++y){
                const float nextHigh=y+1<half?scratch[size_t(half+y+1)*w+x]:high;
                const float nextLow=y+1<half?scratch[size_t(y+1)*w+x]+(-beta)*(nextHigh+high):low;
                image[size_t(y*2)*stride+begin+x]=low;
                image[size_t(y*2+1)*stride+begin+x]=high+(-alpha)*(low+nextLow);
                low=nextLow;high=nextHigh;
            }
        }
        return;
    }
    else{lift(false,alpha);lift(true,beta);lift(false,gamma);lift(true,delta);}
    for(int y=0;y<h;++y){const int source=inverse?(y%2?half+y/2:y/2):y;
        const auto* src=scratch+size_t(source)*w;auto* dst=image+size_t(y)*stride+begin;
        if(!inverse){const auto factor=_mm256_set1_ps(y<half?normal:1/normal);int x=0;
            for(;x+8<=w;x+=8)_mm256_storeu_ps(dst+x,_mm256_mul_ps(_mm256_loadu_ps(src+x),factor));
            for(;x<w;++x)dst[x]=src[x]*(y<half?normal:1/normal);
        }else std::copy_n(src,w,dst);}
}
}


namespace fajk {
// Two RGBA coefficient cells per vector; sums keep the SSE evaluation order.
void sgfBoxVerticalAvx2(const float* a,const float* b,float* outA,float* outB,int w,int h,int r,int y){
 const auto scale=_mm256_set1_ps(1.f/(2*r+1));int x=0;
 for(;x+2<=w;x+=2){auto sa=_mm256_setzero_ps(),sb=sa;for(int dy=-r;dy<=r;++dy){const size_t at=(size_t(std::clamp(y+dy,0,h-1))*w+x)*4;sa=_mm256_add_ps(sa,_mm256_loadu_ps(a+at));sb=_mm256_add_ps(sb,_mm256_loadu_ps(b+at));}const size_t at=(size_t(y)*w+x)*4;_mm256_storeu_ps(outA+at,_mm256_mul_ps(sa,scale));_mm256_storeu_ps(outB+at,_mm256_mul_ps(sb,scale));}
 for(;x<w;++x){auto sa=_mm_setzero_ps(),sb=sa;for(int dy=-r;dy<=r;++dy){const size_t at=(size_t(std::clamp(y+dy,0,h-1))*w+x)*4;sa=_mm_add_ps(sa,_mm_loadu_ps(a+at));sb=_mm_add_ps(sb,_mm_loadu_ps(b+at));}const size_t at=(size_t(y)*w+x)*4;_mm_storeu_ps(outA+at,_mm_mul_ps(sa,_mm_set1_ps(1.f/(2*r+1))));_mm_storeu_ps(outB+at,_mm_mul_ps(sb,_mm_set1_ps(1.f/(2*r+1))));}
}
}
