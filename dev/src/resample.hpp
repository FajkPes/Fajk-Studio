#pragma once
#include "fajk/types.hpp"
#include "fajk/hardware.hpp"
#include <array>
#include <cstring>
#include <immintrin.h>
#include "timing.hpp"
#include "color_boundary.hpp"
#include "gpu_compute.hpp"
namespace fajk {
bool hasAvx2();
bool waveletOpaqueAvx2(const uint8_t*,size_t);
uint32_t resampleVerticalAvx2(const float* const*,const float*,size_t,uint32_t,uint8_t*);
uint32_t resampleLinearAvx2(const uint8_t*,const uint8_t*,const int*,const int*,const float*,const float*,float,float,uint32_t,uint8_t*);
void resampleHorizontalPairsAvx2(const uint8_t*,float*,uint32_t,const int*,const float*,size_t,uint32_t,float*);
inline bool opaquePhoto(std::span<const uint8_t> pixels){
    if(hasAvx2())return waveletOpaqueAvx2(pixels.data(),pixels.size());
    const auto mask=_mm_set1_epi32(int(0xff000000u));size_t i=0;
    for(;i+16<=pixels.size();i+=16)if(_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels.data()+i)),mask),mask))!=65535)return false;
    for(;i<pixels.size();i+=4)if(pixels[i+3]!=255)return false;return true;
}
// Mildly restore channel transitions before low-resolution coding. This keeps
// saturated red/black boundaries from disappearing in the 4:2:0 reduction.
inline Frame preEmphasizePhotoEdges(const Frame& source,uint32_t threads,float amount=.10f,float cleanup=0.f,bool chromaticEdges=false){
    source.validate();require(std::isfinite(amount)&&amount>=0&&amount<=.5f&&std::isfinite(cleanup)&&cleanup>=0&&cleanup<=.75f,"Invalid photo edge prefilter.");const int w=int(source.width),h=int(source.height);const size_t count=size_t(w)*h;
    const auto zero=_mm_setzero_si128();
    auto load=[&](const uint8_t* p){uint32_t rgba;std::memcpy(&rgba,p,4);return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(int(rgba)),zero),zero));};
    auto store=[&](uint8_t* p,__m128 value){
        // Keep lround's half-away-from-zero behavior and the original alpha.
        value=_mm_min_ps(_mm_set1_ps(255.f),_mm_max_ps(_mm_setzero_ps(),value));const auto floor=_mm_cvttps_epi32(value);const auto fraction=_mm_sub_ps(value,_mm_cvtepi32_ps(floor));auto rounded=_mm_sub_epi32(floor,_mm_castps_si128(_mm_cmpge_ps(fraction,_mm_set1_ps(.5f))));rounded=_mm_packus_epi16(_mm_packs_epi32(rounded,zero),zero);const uint32_t rgba=(uint32_t(_mm_cvtsi128_si32(rounded))&0xffffffu)|(uint32_t(p[3])<<24);std::memcpy(p,&rgba,4);
    };
    Frame cleaned;const Frame* input=&source;
    if(cleanup>0){
        cleaned=source;thread_local std::vector<float> lumaCache;std::vector<float> ownedLuma;
        auto& luma=count<=8*1024*1024?lumaCache:ownedLuma;luma.resize(count);
        parallelFor(source.height,threads,[&](uint32_t y){for(int x=0;x<w;++x){const auto* p=source.pixels.data()+(size_t(y)*w+x)*4;luma[size_t(y)*w+x]=.299f*p[0]+.587f*p[1]+.114f*p[2];}});
        parallelFor(source.height,threads,[&](uint32_t y){
            auto scalar=[&](int x){const size_t index=size_t(y)*w+x,at=index*4;const auto center=load(source.pixels.data()+at);auto sum=_mm_mul_ps(center,_mm_set1_ps(4.f));float weight=4.f;
                const size_t neighbors[4]={size_t(y)*w+std::max(0,x-1),size_t(y)*w+std::min(w-1,x+1),size_t(y?y-1:y)*w+x,size_t(std::min(h-1,int(y)+1))*w+x};
                for(auto neighbor:neighbors){const auto color=load(source.pixels.data()+neighbor*4);float difference=std::abs(luma[neighbor]-luma[index]);
                    if(chromaticEdges&&difference<12.f){auto delta=_mm_and_ps(_mm_andnot_ps(_mm_set1_ps(-0.f),_mm_sub_ps(color,center)),_mm_castsi128_ps(_mm_set_epi32(0,-1,-1,-1)));delta=_mm_max_ps(delta,_mm_shuffle_ps(delta,delta,_MM_SHUFFLE(1,0,3,2)));delta=_mm_max_ps(delta,_mm_shuffle_ps(delta,delta,_MM_SHUFFLE(2,3,0,1)));if(_mm_cvtss_f32(delta)>=48.f)difference=24.f;}
                    const float edge=std::max(0.f,1.f-difference/24.f),wt=edge*edge;sum=_mm_add_ps(sum,_mm_mul_ps(color,_mm_set1_ps(wt)));weight+=wt;}
                const auto average=_mm_div_ps(sum,_mm_set1_ps(weight));store(cleaned.pixels.data()+at,_mm_add_ps(_mm_mul_ps(center,_mm_set1_ps(1-cleanup)),_mm_mul_ps(average,_mm_set1_ps(cleanup))));
            };
            auto channels=[&](size_t at,__m128 (&values)[3]){const auto packed=_mm_loadu_si128(reinterpret_cast<const __m128i*>(source.pixels.data()+at*4)),mask=_mm_set1_epi32(255);values[0]=_mm_cvtepi32_ps(_mm_and_si128(packed,mask));values[1]=_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(packed,8),mask));values[2]=_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(packed,16),mask));};
            scalar(0);int x=1;const auto sign=_mm_set1_ps(-0.f),one=_mm_set1_ps(1.f),four=_mm_set1_ps(4.f);
            for(;x+4<w;x+=4){const size_t at=size_t(y)*w+x,neighbors[4]={at-1,at+1,size_t(y?y-1:y)*w+x,size_t(std::min(h-1,int(y)+1))*w+x};__m128 center[3],sum[3];channels(at,center);for(int c=0;c<3;++c)sum[c]=_mm_mul_ps(center[c],four);auto weight=four;const auto light=_mm_loadu_ps(luma.data()+at);
                for(auto neighbor:neighbors){__m128 color[3];channels(neighbor,color);auto difference=_mm_andnot_ps(sign,_mm_sub_ps(_mm_loadu_ps(luma.data()+neighbor),light));
                    if(chromaticEdges){auto colorDifference=_mm_setzero_ps();for(int c=0;c<3;++c)colorDifference=_mm_max_ps(colorDifference,_mm_andnot_ps(sign,_mm_sub_ps(color[c],center[c])));const auto guard=_mm_and_ps(_mm_cmplt_ps(difference,_mm_set1_ps(12.f)),_mm_cmpge_ps(colorDifference,_mm_set1_ps(48.f)));difference=_mm_or_ps(_mm_and_ps(guard,_mm_set1_ps(24.f)),_mm_andnot_ps(guard,difference));}
                    const auto edge=_mm_max_ps(_mm_setzero_ps(),_mm_sub_ps(one,_mm_div_ps(difference,_mm_set1_ps(24.f)))),wt=_mm_mul_ps(edge,edge);for(int c=0;c<3;++c)sum[c]=_mm_add_ps(sum[c],_mm_mul_ps(color[c],wt));weight=_mm_add_ps(weight,wt);
                }
                __m128i rgb[3];for(int c=0;c<3;++c){auto value=_mm_add_ps(_mm_mul_ps(center[c],_mm_set1_ps(1-cleanup)),_mm_mul_ps(_mm_div_ps(sum[c],weight),_mm_set1_ps(cleanup)));value=_mm_min_ps(_mm_set1_ps(255.f),_mm_max_ps(_mm_setzero_ps(),value));const auto floor=_mm_cvttps_epi32(value),round=_mm_castps_si128(_mm_cmpge_ps(_mm_sub_ps(value,_mm_cvtepi32_ps(floor)),_mm_set1_ps(.5f)));rgb[c]=_mm_sub_epi32(floor,round);}
                const auto alpha=_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source.pixels.data()+at*4)),_mm_set1_epi32(int(0xff000000u)));const auto rgba=_mm_or_si128(alpha,_mm_or_si128(rgb[0],_mm_or_si128(_mm_slli_epi32(rgb[1],8),_mm_slli_epi32(rgb[2],16))));_mm_storeu_si128(reinterpret_cast<__m128i*>(cleaned.pixels.data()+at*4),rgba);
            }
            for(;x<w;++x)scalar(x);
        });input=&cleaned;
    }
    if(amount==0)return *input;
    Frame result=*input;thread_local std::vector<float> horizontalCache;std::vector<float> ownedHorizontal;
    auto& horizontal=count<=8*1024*1024?horizontalCache:ownedHorizontal;horizontal.resize(count*4);
    parallelFor(source.height,threads,[&](uint32_t y){const size_t row=size_t(y)*w;for(int x=0;x<w;++x){const auto a=load(input->pixels.data()+(row+std::max(0,x-1))*4),b=load(input->pixels.data()+(row+x)*4),c=load(input->pixels.data()+(row+std::min(w-1,x+1))*4);_mm_storeu_ps(horizontal.data()+(row+x)*4,_mm_mul_ps(_mm_add_ps(_mm_add_ps(a,_mm_mul_ps(b,_mm_set1_ps(2.f))),c),_mm_set1_ps(.25f)));}});
    parallelFor(source.height,threads,[&](uint32_t y){const size_t row=size_t(y)*w,up=size_t(y?y-1:y)*w,down=size_t(std::min(h-1,int(y)+1))*w;for(int x=0;x<w;++x){const auto blurred=_mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_loadu_ps(horizontal.data()+(up+x)*4),_mm_mul_ps(_mm_loadu_ps(horizontal.data()+(row+x)*4),_mm_set1_ps(2.f))),_mm_loadu_ps(horizontal.data()+(down+x)*4)),_mm_set1_ps(.25f));auto* p=result.pixels.data()+(row+x)*4;const auto center=load(p);store(p,_mm_add_ps(center,_mm_mul_ps(_mm_sub_ps(center,blurred),_mm_set1_ps(amount))));}});
    return result;
}
// Opaque photograph experiment. Separable antialiased Lanczos3; source-sized
// output is part of the packet decoding, not a gallery-only enhancement.
inline Frame resamplePhoto(const Frame& source,uint32_t width,uint32_t height,uint32_t threads,std::span<uint8_t> destination={},uint8_t smooth=0,bool boundary=false,bool allowAvx=true){
    TimingTimeline profile;source.validate();require(destination.empty()||destination.size()==pixelBytes(width,height),"Invalid resample destination.");Frame result{width,height,source.pts,source.duration,destination.empty()?Bytes(pixelBytes(width,height)):Bytes{}};const auto pixels=destination.empty()?std::span(result.pixels):destination;
    if(smooth==2&&!boundary&&(width<source.width||height<source.height)&&gpuResample(source,width,height,pixels)){
        profile.mark("encode.resample.gpu");return result;
    }
    struct Tap {int first;std::vector<float> weights;std::vector<int> indices;};
    auto axis=[smooth](uint32_t input,uint32_t output){
        std::vector<Tap> taps(output);const double scale=double(input)/output,stretch=std::max(1.,scale);
        auto sinc=[](double x){if(std::abs(x)<1e-9)return 1.;x*=3.14159265358979323846;return std::sin(x)/x;};
        const double radius=smooth==1?2:smooth==2?1:3;
        for(uint32_t i=0;i<output;++i){double center=(i+.5)*scale-.5;auto& t=taps[i];t.first=int(std::ceil(center-radius*stretch));int last=int(std::floor(center+radius*stretch));double sum=0;
            // Positive weights cannot create sharpening halos or new extrema.
            for(int j=t.first;j<=last;++j){double d=std::abs((j-center)/stretch);double v=smooth==1?(d<1?(4-6*d*d+3*d*d*d)/6:d<2?std::pow(2-d,3)/6:0):smooth==2?std::max(0.,1-d):d<3?sinc(d)*sinc(d/3):0;t.weights.push_back(float(v));sum+=v;}
            for(auto& v:t.weights)v=float(v/sum);
            for(size_t j=0;j<t.weights.size();++j)t.indices.push_back(std::clamp(t.first+int(j),0,int(input)-1));
        }return taps;
    };
    struct AxisCache {uint32_t iw=0,ih=0,ow=0,oh=0;uint8_t smooth=255;std::vector<Tap> horizontal,vertical;size_t pairTaps=0;std::vector<int> pairIndices;std::vector<float> pairWeights;};
    thread_local std::array<AxisCache,2> axisCaches;
    auto& axes=axisCaches[width<source.width||height<source.height?0:1];
    if(axes.iw!=source.width||axes.ih!=source.height||axes.ow!=width||axes.oh!=height||axes.smooth!=smooth){
        axes.horizontal=axis(source.width,width);axes.vertical=axis(source.height,height);
        axes.pairTaps=0;axes.pairIndices.clear();axes.pairWeights.clear();
        if(smooth==2&&width<source.width){
            for(const auto& tap:axes.horizontal)axes.pairTaps=std::max(axes.pairTaps,tap.weights.size());
            axes.pairIndices.resize(size_t(width)*axes.pairTaps);axes.pairWeights.assign(axes.pairIndices.size(),0.f);
            for(size_t x=0;x<width;++x)for(size_t j=0;j<axes.horizontal[x].weights.size();++j){axes.pairIndices[x*axes.pairTaps+j]=axes.horizontal[x].indices[j];axes.pairWeights[x*axes.pairTaps+j]=axes.horizontal[x].weights[j];}
        }
        axes.iw=source.width;axes.ih=source.height;axes.ow=width;axes.oh=height;axes.smooth=smooth;
    }
    const auto& horizontal=axes.horizontal;const auto& vertical=axes.vertical;
    if(smooth==2&&width>=source.width&&height>=source.height){
        struct LinearCache{uint32_t input=0,output=0;std::vector<int> low,high;std::vector<float> left,right;};thread_local LinearCache line;
        if(line.input!=source.width||line.output!=width){line.low.resize(width);line.high.resize(width);line.left.resize(width);line.right.resize(width);for(uint32_t x=0;x<width;++x){const auto& t=horizontal[x];size_t a=0,b=t.weights.size()-1;while(t.weights[a]==0)++a;while(t.weights[b]==0)--b;line.low[x]=t.indices[a];line.high[x]=t.indices[b];line.left[x]=t.weights[a];line.right[x]=a==b?0:t.weights[b];}line.input=source.width;line.output=width;}
        const auto& low=line.low;const auto& high=line.high;const auto& left=line.left;const auto& right=line.right;
        // Same-size linear reconstruction is an identity for opaque pixels.
        // Keep the boundary stripe path, but skip all interpolation arithmetic.
        const bool copyRows=width==source.width&&height==source.height&&opaquePhoto(source.pixels);
        const bool avx=allowAvx&&hasAvx2();auto row=[&](uint32_t y,uint8_t* out){if(copyRows){std::copy_n(source.pixels.data()+size_t(y)*width*4,size_t(width)*4,out);return;}const auto& t=vertical[y];size_t a=0,b=t.weights.size()-1;while(t.weights[a]==0)++a;while(t.weights[b]==0)--b;const float up=t.weights[a],down=a==b?0:t.weights[b];const auto* top=source.pixels.data()+size_t(t.indices[a])*source.width*4;const auto* bottom=source.pixels.data()+size_t(t.indices[b])*source.width*4;
            uint32_t x=avx?resampleLinearAvx2(top,bottom,low.data(),high.data(),left.data(),right.data(),up,down,width,out):0;const auto zero=_mm_setzero_si128();auto load=[&](const uint8_t* row,int at){uint32_t v;std::memcpy(&v,row+size_t(at)*4,4);return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(int(v)),zero),zero));};
            for(;x<width;++x){auto upper=_mm_add_ps(_mm_mul_ps(load(top,low[x]),_mm_set1_ps(left[x])),_mm_mul_ps(load(top,high[x]),_mm_set1_ps(right[x])));auto lower=_mm_add_ps(_mm_mul_ps(load(bottom,low[x]),_mm_set1_ps(left[x])),_mm_mul_ps(load(bottom,high[x]),_mm_set1_ps(right[x])));auto value=_mm_add_ps(_mm_mul_ps(upper,_mm_set1_ps(up)),_mm_mul_ps(lower,_mm_set1_ps(down)));auto v=_mm_cvttps_epi32(_mm_add_ps(value,_mm_set1_ps(.5f)));auto packed=_mm_packus_epi16(_mm_packs_epi32(v,zero),zero);uint32_t rgba=uint32_t(_mm_cvtsi128_si32(packed))|0xff000000u;std::memcpy(out+size_t(x)*4,&rgba,4);}
        };
        if(boundary){
            // Keep interpolation and 4342 in one worker-local stripe. Halo rows
            // are read-only; their three-pixel radius is identical to a full frame.
            constexpr uint32_t group=32;parallelFor((height+group-1)/group,threads,[&](uint32_t job){
                const uint32_t begin=job*group,end=std::min(height,begin+group),first=begin>3?begin-3:0,last=std::min(height,end+3);
                thread_local Frame stripe;thread_local Bytes filtered;stripe.width=width;stripe.height=last-first;stripe.pts=source.pts;stripe.duration=source.duration;stripe.pixels.resize(pixelBytes(width,stripe.height));filtered.resize(stripe.pixels.size());
                for(uint32_t y=first;y<last;++y)row(y,stripe.pixels.data()+size_t(y-first)*width*4);
                restoreColorBoundaries(stripe,1,1.f,3.f,2,0,false,true,filtered,begin-first,end-first);
                std::copy_n(filtered.data()+size_t(begin-first)*width*4,size_t(end-begin)*width*4,pixels.data()+size_t(begin)*width*4);
            });
        }else parallelFor(height,threads,[&](uint32_t y){row(y,pixels.data()+size_t(y)*width*4);});
        profile.mark("decode.resample.direct_linear");return result;
    }
    if(smooth&&width>=source.width&&height>=source.height){
        // Keep the few overlapping rows in each worker's cache instead of
        // writing and rereading a full-size floating-point intermediate.
        const bool avx=allowAvx&&hasAvx2();const uint32_t group=16;
        parallelFor((height+group-1)/group,threads,[&](uint32_t job){
            const uint32_t begin=job*group,end=std::min(height,begin+group);
            const int first=vertical[begin].indices.front(),last=vertical[end-1].indices.back();
            thread_local std::vector<float> stripe,converted;stripe.resize(size_t(last-first+1)*width*4);converted.resize(size_t(source.width)*4);
            const auto zero=_mm_setzero_si128();
            for(int y=first;y<=last;++y){
                for(uint32_t x=0;x<source.width;++x){uint32_t rgba;std::memcpy(&rgba,source.pixels.data()+(size_t(y)*source.width+x)*4,4);_mm_storeu_ps(converted.data()+size_t(x)*4,_mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(int(rgba)),zero),zero)));}
                for(uint32_t x=0;x<width;++x){const auto& t=horizontal[x];auto value=_mm_setzero_ps();for(size_t j=0;j<t.weights.size();++j)value=_mm_add_ps(value,_mm_mul_ps(_mm_loadu_ps(converted.data()+size_t(t.indices[j])*4),_mm_set1_ps(t.weights[j])));_mm_storeu_ps(stripe.data()+(size_t(y-first)*width+x)*4,value);}
            }
            thread_local std::vector<const float*> rows;
            for(uint32_t y=begin;y<end;++y){const auto& t=vertical[y];rows.resize(t.indices.size());for(size_t j=0;j<rows.size();++j)rows[j]=stripe.data()+size_t(t.indices[j]-first)*width*4;
                uint32_t x=avx?resampleVerticalAvx2(rows.data(),t.weights.data(),rows.size(),width,pixels.data()+size_t(y)*width*4):0;
                for(;x<width;++x){auto value=_mm_setzero_ps();for(size_t j=0;j<rows.size();++j)value=_mm_add_ps(value,_mm_mul_ps(_mm_loadu_ps(rows[j]+size_t(x)*4),_mm_set1_ps(t.weights[j])));auto v=_mm_cvttps_epi32(_mm_add_ps(_mm_min_ps(_mm_set1_ps(255),_mm_max_ps(_mm_setzero_ps(),value)),_mm_set1_ps(.5f)));auto packed=_mm_packus_epi16(_mm_packs_epi32(v,zero),zero);uint32_t rgba=uint32_t(_mm_cvtsi128_si32(packed))|0xff000000u;std::memcpy(pixels.data()+(size_t(y)*width+x)*4,&rgba,4);}
            }
        });profile.mark("decode.resample.cached_stripes");return result;
    }
    thread_local std::vector<float> cache;std::vector<float> owned;auto& intermediate=size_t(width)*source.height<=8*1024*1024?cache:owned;intermediate.resize(size_t(width)*source.height*4);
    const bool down=width<source.width||height<source.height;
    const bool resampleAvx2=allowAvx&&hasAvx2();
    profile.mark(down?"encode.resample.prepare":"decode.resample.prepare");
    parallelFor(source.height,threads,[&](uint32_t y){
        // Convert each source pixel once instead of once per overlapping tap.
        thread_local std::vector<float> converted;converted.resize(size_t(source.width)*4);
        if(resampleAvx2&&axes.pairTaps){resampleHorizontalPairsAvx2(source.pixels.data()+size_t(y)*source.width*4,converted.data(),source.width,axes.pairIndices.data(),axes.pairWeights.data(),axes.pairTaps,width,intermediate.data()+size_t(y)*width*4);return;}
        const auto zero=_mm_setzero_si128();
        for(uint32_t x=0;x<source.width;++x){uint32_t rgba;std::memcpy(&rgba,source.pixels.data()+(size_t(y)*source.width+x)*4,4);const auto v=_mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(int(rgba)),zero),zero));_mm_storeu_ps(converted.data()+size_t(x)*4,v);}
        for(uint32_t x=0;x<width;++x){const auto& t=horizontal[x];auto value=_mm_setzero_ps();for(size_t j=0;j<t.weights.size();++j){const auto v=_mm_loadu_ps(converted.data()+size_t(t.indices[j])*4);value=_mm_add_ps(value,_mm_mul_ps(v,_mm_set1_ps(t.weights[j])));}_mm_storeu_ps(intermediate.data()+(size_t(y)*width+x)*4,value);}
    });
    profile.mark(down?"encode.resample.horizontal":"decode.resample.horizontal");
    auto toInt=[](__m128 v){return _mm_cvttps_epi32(_mm_add_ps(_mm_min_ps(_mm_set1_ps(255),_mm_max_ps(_mm_setzero_ps(),v)),_mm_set1_ps(.5f)));};
    parallelFor(height,threads,[&](uint32_t y){const auto& t=vertical[y];uint32_t x=0;if(resampleAvx2){thread_local std::vector<const float*> rows;rows.resize(t.indices.size());for(size_t j=0;j<rows.size();++j)rows[j]=intermediate.data()+size_t(t.indices[j])*width*4;x=resampleVerticalAvx2(rows.data(),t.weights.data(),rows.size(),width,pixels.data()+size_t(y)*width*4);}for(;x+4<=width;x+=4){auto a=_mm_setzero_ps(),b=a,c=a,d=a;for(size_t j=0;j<t.weights.size();++j){const auto* row=intermediate.data()+(size_t(t.indices[j])*width+x)*4;auto weight=_mm_set1_ps(t.weights[j]);a=_mm_add_ps(a,_mm_mul_ps(_mm_loadu_ps(row),weight));b=_mm_add_ps(b,_mm_mul_ps(_mm_loadu_ps(row+4),weight));c=_mm_add_ps(c,_mm_mul_ps(_mm_loadu_ps(row+8),weight));d=_mm_add_ps(d,_mm_mul_ps(_mm_loadu_ps(row+12),weight));}auto packed=_mm_packus_epi16(_mm_packs_epi32(toInt(a),toInt(b)),_mm_packs_epi32(toInt(c),toInt(d)));packed=_mm_or_si128(packed,_mm_set1_epi32(int(0xff000000u)));_mm_storeu_si128(reinterpret_cast<__m128i*>(pixels.data()+(size_t(y)*width+x)*4),packed);}
        for(;x<width;++x){auto value=_mm_setzero_ps();for(size_t j=0;j<t.weights.size();++j)value=_mm_add_ps(value,_mm_mul_ps(_mm_loadu_ps(intermediate.data()+(size_t(t.indices[j])*width+x)*4),_mm_set1_ps(t.weights[j])));auto packed=_mm_packus_epi16(_mm_packs_epi32(toInt(value),_mm_setzero_si128()),_mm_setzero_si128());uint32_t rgba=uint32_t(_mm_cvtsi128_si32(packed))|0xff000000u;std::memcpy(pixels.data()+(size_t(y)*width+x)*4,&rgba,4);}});
    profile.mark(down?"encode.resample.vertical":"decode.resample.vertical");
    return result;
}
}
