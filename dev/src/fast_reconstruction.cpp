#include "fast_reconstruction.hpp"
#include "reconstruction441.hpp"
#include "cleanup_quality.hpp"
#include "tangent_cleanup.hpp"
#include "fajk/hardware.hpp"
#include "timing.hpp"
#include "flat_regions.hpp"
#include "text_guard.hpp"
#include "gpu_compute.hpp"
#include <array>
#include <immintrin.h>
namespace fajk {
namespace {
#include "contour_curvature_avx2.inl"
#include "weak_cleanup_avx2.inl"
inline void transpose8(__m256 (&r)[8]){
    auto t0=_mm256_unpacklo_ps(r[0],r[1]),t1=_mm256_unpackhi_ps(r[0],r[1]),t2=_mm256_unpacklo_ps(r[2],r[3]),t3=_mm256_unpackhi_ps(r[2],r[3]);
    auto t4=_mm256_unpacklo_ps(r[4],r[5]),t5=_mm256_unpackhi_ps(r[4],r[5]),t6=_mm256_unpacklo_ps(r[6],r[7]),t7=_mm256_unpackhi_ps(r[6],r[7]);
    const __m256 v[8]{_mm256_shuffle_ps(t0,t2,0x44),_mm256_shuffle_ps(t0,t2,0xee),_mm256_shuffle_ps(t1,t3,0x44),_mm256_shuffle_ps(t1,t3,0xee),
                       _mm256_shuffle_ps(t4,t6,0x44),_mm256_shuffle_ps(t4,t6,0xee),_mm256_shuffle_ps(t5,t7,0x44),_mm256_shuffle_ps(t5,t7,0xee)};
    for(int i=0;i<4;++i){r[i]=_mm256_permute2f128_ps(v[i],v[i+4],0x20);r[i+4]=_mm256_permute2f128_ps(v[i],v[i+4],0x31);}
}
}
void fastEdgeUpscaleRow(const uint8_t*,uint32_t,uint32_t,uint8_t*,uint32_t,const int*,const float*,int,float,float);
template<bool fused> static void recursiveContourCleanupImpl(std::span<uint8_t> pixels,uint32_t w,uint32_t h,uint32_t workers,float amount,float stronger,std::span<const uint8_t> input,float curvature,std::span<const uint8_t> skipTiles){
    require(pixels.size()==pixelBytes(w,h)&&amount>=0&&amount<=1&&stronger>=0&&stronger<=1,"Invalid recursive cleanup");
    require(curvature>=0&&curvature<=1,"Invalid contour regularization");
    if(!amount)return;
    require(input.empty()||input.size()==pixels.size(),"Invalid recursive source");
    // With a small CPU budget offloading wins including both RGBA transfers.
    // At larger budgets AVX2 is faster: do not add a GPU round trip merely
    // because the GPU backend is selected. An unsuccessful offload is atomic.
    if(workers<=2&&gpuContourCleanup(pixels,input.empty()?std::span<const uint8_t>(pixels):input,w,h,amount,stronger,curvature,skipTiles,fused))return;
    thread_local Bytes cachedSource;
    Bytes largeSource;
    auto& owned=pixels.size()<=32*1024*1024?cachedSource:largeSource;
    if(input.empty())owned.assign(pixels.begin(),pixels.end());
    const auto source=input.empty()?std::span<const uint8_t>(owned):input;
    if(amount<=.2f&&stronger==0&&curvature<=.06f){weakCleanup(pixels,source,w,h,workers,amount);return;}
    const float range=32+stronger*20;
    constexpr uint32_t tile=recursiveCleanupTile,halo=8;const uint32_t cols=(w+tile-1)/tile,rows=(h+tile-1)/tile;
    require(skipTiles.empty()||skipTiles.size()==size_t(cols)*rows,"Invalid flat cleanup mask.");
    parallelFor(cols*rows,workers,[&](uint32_t job){
        if(!skipTiles.empty()&&skipTiles[job])return;
        const uint32_t x0=(job%cols)*tile,y0=(job/cols)*tile,x1=std::min(w,x0+tile),y1=std::min(h,y0+tile);
        const uint32_t left=x0>halo?x0-halo:0,top=y0>halo?y0-halo:0,right=std::min(w,x1+halo),bottom=std::min(h,y1+halo),tw=right-left,th=bottom-top;
        uint32_t firstColor;std::memcpy(&firstColor,source.data()+(size_t(top)*w+left)*4,4);
        bool uniform=true;const auto firstVector=_mm256_set1_epi32(int(firstColor));
        for(uint32_t y=top;y<bottom&&uniform;++y){const auto* row=source.data()+(size_t(y)*w+left)*4;uint32_t x=0;
            for(;x+8<=tw;x+=8)if(_mm256_movemask_epi8(_mm256_cmpeq_epi32(firstVector,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(row+x*4))))!=-1){uniform=false;break;}
            for(;x<tw&&uniform;++x){uint32_t color;std::memcpy(&color,row+x*4,4);uniform=color==firstColor;}}
        if(uniform)return;
        const uint32_t pitch=(tw+7)&~7u,ypitch=(th+7)&~7u;const size_t area=size_t(pitch)*ypitch;
        // Most tiles are square. Transpose those in place so three RGB planes,
        // rather than six, remain in the worker's cache throughout the filter.
        const bool square=pitch==ypitch;
        thread_local std::vector<float> scratch;scratch.resize(area*(square?3:6));
        thread_local std::vector<uint32_t> guide;guide.resize(area);
        float* transposed[3]{scratch.data(),scratch.data()+area,scratch.data()+area*2};
        float* planes[3]{scratch.data()+area*(square?0:3),scratch.data()+area*(square?1:4),scratch.data()+area*(square?2:5)};
        const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1);const auto mask=_mm256_set1_epi32(255);
        auto unpack=[&](__m256i p,int ch){return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(p,_mm256_set1_epi32(ch*8)),mask));};
        auto coefficient=[&](__m256i a,__m256i b){const auto d=_mm256_or_si256(_mm256_subs_epu8(a,b),_mm256_subs_epu8(b,a));
            const auto maximum=_mm256_max_epu8(d,_mm256_max_epu8(_mm256_srli_epi32(d,8),_mm256_srli_epi32(d,16)));
            const auto r=_mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(maximum,mask)),_mm256_set1_ps(1/range));
            const auto curve=_mm256_max_ps(zero,_mm256_sub_ps(one,_mm256_mul_ps(r,r)));
            auto lambda=_mm256_mul_ps(_mm256_set1_ps(.78f),_mm256_mul_ps(curve,curve));
            return _mm256_and_ps(lambda,_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_srli_epi32(_mm256_and_si256(a,b),24),mask)));};
        for(uint32_t y=0;y<ypitch;y+=8){
            for(uint32_t x=0;x<pitch;x+=8){
                __m256 block[8];
                for(uint32_t dy=0;dy<8;++dy){const auto* row=reinterpret_cast<const int*>(source.data()+size_t(top+std::min(y+dy,th-1))*w*4);
                    __m256i packed;
                    if(x+8<=tw)packed=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(row+left+x));
                    else {const auto indices=_mm256_min_epi32(_mm256_set1_epi32(right-1),_mm256_add_epi32(_mm256_set1_epi32(left+x),_mm256_setr_epi32(0,1,2,3,4,5,6,7)));packed=_mm256_i32gather_epi32(row,indices,4);}
                    block[dy]=_mm256_castsi256_ps(packed);
                }
                transpose8(block);
                for(int dx=0;dx<8;++dx){const auto packed=_mm256_castps_si256(block[dx]);const auto at=size_t(x+dx)*ypitch+y;
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(guide.data()+at),packed);
                    for(int c=0;c<3;++c)_mm256_storeu_ps(transposed[c]+at,unpack(packed,c));
                }
            }
        }
        // Transposed tiles turn the horizontal recurrence into eight fully
        // independent rows. No per-pixel prefix scan, gather, or scalar RGB.
        auto recurrence=[](__m256 own,__m256 lambda,__m256 previous){
            if constexpr(fused)return _mm256_fmadd_ps(lambda,_mm256_sub_ps(previous,own),own);
            else return _mm256_add_ps(own,_mm256_mul_ps(lambda,_mm256_sub_ps(previous,own)));
        };
        auto horizontal=[&](bool backwards){for(uint32_t y=0;y<ypitch;y+=8){
            const int begin=backwards?int(pitch)-1:0,end=backwards?-1:int(pitch),step=backwards?-1:1;
            __m256 value[3];for(int c=0;c<3;++c)value[c]=_mm256_loadu_ps(transposed[c]+size_t(begin)*ypitch+y);
            auto previous=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(guide.data()+size_t(begin)*ypitch+y));
            for(int x=begin+step;x!=end;x+=step){const size_t at=size_t(x)*ypitch+y;
                __m256 lambda;
                if(backwards)lambda=_mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(guide.data()+at+ypitch)));
                else{const auto packed=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(guide.data()+at));lambda=coefficient(packed,previous);previous=packed;
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(guide.data()+at),_mm256_castps_si256(lambda));}
                for(int c=0;c<3;++c){auto own=_mm256_loadu_ps(transposed[c]+at);value[c]=recurrence(own,lambda,value[c]);_mm256_storeu_ps(transposed[c]+at,value[c]);}
            }
        }};horizontal(false);horizontal(true);
        // Cache-local 8x8 transpose; vertical recurrences then use contiguous
        // vectors as well. Both buffers fit into a worker's L2 cache.
        if(square){
            for(int c=0;c<3;++c)for(uint32_t y=0;y<pitch;y+=8)for(uint32_t x=y;x<pitch;x+=8){
                __m256 a[8],b[8];
                for(int i=0;i<8;++i)a[i]=_mm256_loadu_ps(planes[c]+size_t(y+i)*pitch+x);
                if(x!=y)for(int i=0;i<8;++i)b[i]=_mm256_loadu_ps(planes[c]+size_t(x+i)*pitch+y);
                transpose8(a);
                if(x!=y)transpose8(b);
                for(int i=0;i<8;++i)_mm256_storeu_ps(planes[c]+size_t(x+i)*pitch+y,a[i]);
                if(x!=y)for(int i=0;i<8;++i)_mm256_storeu_ps(planes[c]+size_t(y+i)*pitch+x,b[i]);
            }
        }else for(int c=0;c<3;++c)for(uint32_t y=0;y<ypitch;y+=8)for(uint32_t x=0;x<pitch;x+=8){
            __m256 r[8];for(int i=0;i<8;++i)r[i]=_mm256_loadu_ps(transposed[c]+size_t(x+i)*ypitch+y);
            transpose8(r);for(int i=0;i<8;++i)_mm256_storeu_ps(planes[c]+size_t(y+i)*pitch+x,r[i]);
        }
        auto rawRow=[&](uint32_t x,uint32_t y){const auto* p=source.data()+(size_t(top+std::min(y,th-1))*w+left+x)*4;
            if(x+8<=tw)return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            auto offsets=_mm256_add_epi32(_mm256_set1_epi32(left+x),_mm256_setr_epi32(0,1,2,3,4,5,6,7));offsets=_mm256_min_epi32(offsets,_mm256_set1_epi32(right-1));
            return _mm256_i32gather_epi32(reinterpret_cast<const int*>(source.data()+size_t(top+std::min(y,th-1))*w*4),offsets,4);};
        auto vertical=[&](bool backwards){for(uint32_t x=0;x<pitch;x+=8){
            const int begin=backwards?int(ypitch)-1:0,end=backwards?-1:int(ypitch),step=backwards?-1:1;
            __m256 value[3];for(int c=0;c<3;++c)value[c]=_mm256_loadu_ps(planes[c]+size_t(begin)*pitch+x);auto previous=rawRow(x,begin);
            for(int y=begin+step;y!=end;y+=step){const size_t at=size_t(y)*pitch+x;
                __m256 lambda;
                if(backwards)lambda=_mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(guide.data()+at+pitch)));
                else{const auto packed=rawRow(x,y);lambda=coefficient(packed,previous);previous=packed;
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(guide.data()+at),_mm256_castps_si256(lambda));}
                for(int c=0;c<3;++c){auto own=_mm256_loadu_ps(planes[c]+at);value[c]=recurrence(own,lambda,value[c]);_mm256_storeu_ps(planes[c]+at,value[c]);}
            }
        }};vertical(false);vertical(true);
        for(uint32_t y=y0;y<y1;++y){uint32_t x=x0;
            for(;x+8<=x1;x+=8){const auto packed=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(source.data()+(size_t(y)*w+x)*4));auto out=_mm256_and_si256(packed,_mm256_set1_epi32(int(0xff000000u)));
                __m256 clean[3];contourTangent8(planes,pitch,tw,th,x-left,y-top,curvature,clean);
                for(int c=0;c<3;++c){auto original=unpack(packed,c);auto v=_mm256_add_ps(original,_mm256_mul_ps(_mm256_set1_ps(amount),_mm256_sub_ps(clean[c],original)));auto channel=_mm256_cvttps_epi32(_mm256_add_ps(_mm256_set1_ps(.5f),_mm256_min_ps(_mm256_set1_ps(255),_mm256_max_ps(zero,v))));out=_mm256_or_si256(out,_mm256_sllv_epi32(channel,_mm256_set1_epi32(c*8)));}
                out=_mm256_blendv_epi8(packed,out,_mm256_cmpeq_epi32(_mm256_srli_epi32(packed,24),mask));_mm256_storeu_si256(reinterpret_cast<__m256i*>(pixels.data()+(size_t(y)*w+x)*4),out);}
            if(x<x1){__m256 clean[3];contourTangent8(planes,pitch,tw,th,x-left,y-top,curvature,clean);alignas(32)float tail[3][8];for(int c=0;c<3;++c)_mm256_store_ps(tail[c],clean[c]);
                for(uint32_t lane=0;x<x1;++x,++lane){auto* out=pixels.data()+(size_t(y)*w+x)*4;const auto* original=source.data()+(size_t(y)*w+x)*4;if(original[3]!=255)continue;for(int c=0;c<3;++c)out[c]=uint8_t(std::clamp(int(original[c]+amount*(tail[c][lane]-original[c])+.5f),0,255));}}
        }
    });_mm256_zeroupper();
}
void recursiveContourCleanup(std::span<uint8_t> pixels,uint32_t w,uint32_t h,uint32_t workers,float amount,float stronger,std::span<const uint8_t> input,float curvature,std::span<const uint8_t> skipTiles,bool fusedArithmetic){
    if(fusedArithmetic&&hasFma())recursiveContourCleanupImpl<true>(pixels,w,h,workers,amount,stronger,input,curvature,skipTiles);
    else recursiveContourCleanupImpl<false>(pixels,w,h,workers,amount,stronger,input,curvature,skipTiles);
}
namespace {
struct UpscaleGeometry {
    std::span<const int> xLow,yLow;
    std::span<const float> xFraction,yFraction;
    float sharpness;
};
UpscaleGeometry upscaleGeometry(const Frame& native,uint32_t width,uint32_t height,uint32_t budget,
                                const Reconstruction441& settings,float amount,const CleanupQuality& quality){
    struct Axis{uint32_t input=0,output=0;std::vector<int> low;std::vector<float> fraction;};thread_local Axis horizontal,vertical;
    auto prepare=[](Axis& a,uint32_t input,uint32_t output){if(a.input==input&&a.output==output)return;a.low.resize(output);a.fraction.resize(output);for(uint32_t i=0;i<output;++i){const double p=(i+.5)*input/output-.5;a.low[i]=int(std::floor(p));a.fraction[i]=float(p-a.low[i]);}a.input=input;a.output=output;};
    prepare(horizontal,native.width,width);prepare(vertical,native.height,height);
    // Bind caller-owned TLS before scheduling: workers have different TLS.
    const auto& xLow=horizontal.low;const auto& xFraction=horizontal.fraction;
    const auto& yLow=vertical.low;const auto& yFraction=vertical.fraction;
    // AA integrates the transition rather than adding a separate full-frame
    // pass. 50% preserves the reviewed recursive-avx transition width.
    const float aaWidth=1.2f-.4f*settings.antialias/100.f;
    // Moderate rates benefit from a slightly narrower transition after cleaning.
    // Keep both endpoints (disabled and the approved 30 kB setting) unchanged.
    // The resampler still bounds RGB to its source neighborhood: no overshoot.
    const float edgeAmount=amount*(1.4f-.4f*amount);
    const double density=double(budget)*double(2560*1440)/(double(width)*height);
    const float detail=std::clamp(float((density-40000.)/40000.),0.f,1.f);
    // Cleanup fades with quality, but interpolation must not fade to soft
    // bilinear at the same time. Narrow transitions using the existing taps.
    const float sharpness=std::clamp(edgeAmount*(settings.style==0?1.35f:.9f)*aaWidth+.48f*detail+settings.contrast/100.f,0.f,1.5f);
    return {xLow,yLow,xFraction,yFraction,sharpness};
}
}
Frame fastReconstruct441(Frame& native,uint32_t width,uint32_t height,uint32_t workers,uint32_t budget,const Reconstruction441& settings,std::span<uint8_t> destination,const FlatRegionLayout* flat,const DecodedTextProtection* protectedText){
    native.validate();validate441(settings);
    require(width>=native.width&&height>=native.height,"Invalid reconstruction dimensions");
    require(destination.empty()||destination.size()==pixelBytes(width,height),"Invalid fast reconstruction output");
    const auto quality=cleanupQuality(budget,width,height);const float amount=std::clamp(quality.contours*settings.intensity/100.f,0.f,1.f);
    TimingTimeline timing,parts;
    Bytes skipTiles;
    if(flat&&amount>.2f){
        constexpr uint32_t tile=recursiveCleanupTile;const uint32_t cols=(native.width+tile-1)/tile,rows=(native.height+tile-1)/tile;
        skipTiles.resize(size_t(cols)*rows);
        for(uint32_t ty=0;ty<rows;++ty)for(uint32_t tx=0;tx<cols;++tx){
            // Every output influenced by this tile, including interpolation
            // support, must later be replaced by an exact stored flat color.
            auto lower=[](uint32_t at,uint32_t input,uint32_t output){return uint32_t(std::max(0.,std::floor((double(at)-4)*output/input)-2));};
            auto upper=[](uint32_t at,uint32_t input,uint32_t output){return uint32_t(std::min(double(output),std::ceil((double(at)+4)*output/input)+2));};
            const auto x0=lower(tx*tile,native.width,width),y0=lower(ty*tile,native.height,height);
            const auto x1=upper(std::min(native.width,(tx+1)*tile),native.width,width),y1=upper(std::min(native.height,(ty+1)*tile),native.height,height);
            bool covered=true;
            for(uint32_t y=y0/flat->tile;covered&&y<=(y1-1)/flat->tile;++y)
                for(uint32_t x=x0/flat->tile;x<=(x1-1)/flat->tile;++x)if(!flat->map[size_t(y)*flat->cols+x]){covered=false;break;}
            skipTiles[size_t(ty)*cols+tx]=uint8_t(covered);
        }
    }
    // The joined readback wins for 1440p, but 4K output was slower than AVX2
    // and failed the stricter source-relative quality gate. Keep that CPU path.
    if(!settings.protectText&&amount>0&&uint64_t(width)*height<=4*1024*1024&&computeBackend()==ComputeBackend::Gpu){
        Frame accelerated{width,height,native.pts,native.duration,destination.empty()?Bytes(pixelBytes(width,height)):Bytes{}};
        auto pixels=destination.empty()?std::span(accelerated.pixels):destination;
        const auto geometry=upscaleGeometry(native,width,height,budget,settings,amount,quality);
        if(gpuContourUpscale(native,width,height,pixels,geometry.xLow,geometry.xFraction,geometry.yLow,geometry.yFraction,
            geometry.sharpness,amount,settings.strongerTangents/100.f,.4f*quality.antialias*settings.antialias/50.f,skipTiles,hasFma())){
            timing.mark("decode.fast441.contour_upscale.gpu");return accelerated;
        }
    }
    thread_local Bytes snapshot;Bytes largeSnapshot;
    auto& original=native.pixels.size()<=32*1024*1024?snapshot:largeSnapshot;
    if(settings.protectText&&amount)original.assign(native.pixels.begin(),native.pixels.end());
    parts.mark("decode.fast441.snapshot");
    recursiveContourCleanup(native.pixels,native.width,native.height,workers,amount,settings.strongerTangents/100.f,settings.protectText&&amount?std::span<const uint8_t>(original):std::span<const uint8_t>{},.4f*quality.antialias*settings.antialias/50.f,skipTiles);
    parts.mark("decode.fast441.recurrence");
    if(settings.protectText){
        if(protectedText)restoreDecodedText(native.pixels,*protectedText,native.width,native.height);
        else if(amount)preserveDecodedText(native.pixels,original,native.width,native.height,workers);
    }
    parts.mark("decode.fast441.text");
    timing.mark("decode.fast441.cleanup");
    Frame out{width,height,native.pts,native.duration,destination.empty()?Bytes(pixelBytes(width,height)):Bytes{}};
    parts.mark("decode.fast441.output_allocation");
    timing.mark("decode.fast441.allocate");
    auto pixels=destination.empty()?std::span(out.pixels):destination;
    const auto geometry=upscaleGeometry(native,width,height,budget,settings,amount,quality);
    const auto xLow=geometry.xLow,yLow=geometry.yLow;
    const auto xFraction=geometry.xFraction,yFraction=geometry.yFraction;
    const float sharpness=geometry.sharpness;
    // The packed output transfer wins against a small CPU budget. With more
    // workers the existing AVX2 interpolation is faster end to end.
    if(workers<=2&&gpuEdgeUpscale(native,width,height,pixels,xLow,xFraction,yLow,yFraction,sharpness)){
        timing.mark("decode.fast441.upscale.gpu");return out;
    }
    parallelFor((height+15)/16,workers,[&](uint32_t job){for(uint32_t y=job*16;y<std::min(height,(job+1)*16);++y)
        fastEdgeUpscaleRow(native.pixels.data(),native.width,native.height,pixels.data()+size_t(y)*width*4,width,xLow.data(),xFraction.data(),yLow[y],yFraction[y],sharpness);});
    timing.mark("decode.fast441.upscale");return out;
}
}
