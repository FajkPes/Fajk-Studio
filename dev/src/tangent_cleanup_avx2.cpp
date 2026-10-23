#include "tangent_cleanup.hpp"
#include "fajk/hardware.hpp"
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include "coherent_reference441.hpp"
namespace fajk {
namespace {
struct Guide {std::array<float,3> rgb{};float xx=0,xy=0,yy=0;};
}
void tangentCleanup(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float strength,float radius,float range){
    if(pixels.size()!=pixelBytes(width,height)||!std::isfinite(strength)||strength<0||strength>1||!std::isfinite(radius)||radius<1||radius>8||!std::isfinite(range)||range<1||range>64)throw std::runtime_error("Invalid tangent cleanup controls.");
    if(!strength)return;
    const Bytes source(pixels.begin(),pixels.end());constexpr uint32_t grid=8;const uint32_t cols=(width+grid-1)/grid,rows=(height+grid-1)/grid;std::vector<Guide> guide(size_t(cols)*rows),tensor(guide.size());
    parallelFor(rows,workers,[&](uint32_t cy){for(uint32_t cx=0;cx<cols;++cx){auto& g=guide[size_t(cy)*cols+cx];float count=0;for(uint32_t y=cy*grid;y<std::min(height,(cy+1)*grid);++y)for(uint32_t x=cx*grid;x<std::min(width,(cx+1)*grid);++x){const auto* p=source.data()+(size_t(y)*width+x)*4;for(int c=0;c<3;++c)g.rgb[c]+=p[c];++count;}for(auto& v:g.rgb)v/=count;}});
    parallelFor(rows,workers,[&](uint32_t y){for(uint32_t x=0;x<cols;++x){auto& t=tensor[size_t(y)*cols+x];for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const auto xx=uint32_t(std::clamp(int(x)+dx,0,int(cols)-1)),yy=uint32_t(std::clamp(int(y)+dy,0,int(rows)-1));const auto& a=guide[size_t(yy)*cols+(xx?xx-1:xx)].rgb;const auto& b=guide[size_t(yy)*cols+std::min(xx+1,cols-1)].rgb;const auto& c=guide[size_t(yy?yy-1:yy)*cols+xx].rgb;const auto& d=guide[size_t(std::min(yy+1,rows-1))*cols+xx].rgb;for(int k=0;k<3;++k){const float gx=(b[k]-a[k])/16,gy=(d[k]-c[k])/16;t.xx+=gx*gx/9;t.xy+=gx*gy/9;t.yy+=gy*gy/9;}}}});
    parallelFor(height,workers,[&](uint32_t y){
        const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1),half=_mm256_set1_ps(.5f),maskAbs=_mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));auto abs=[&](__m256 v){return _mm256_and_ps(v,maskAbs);};auto sat=[&](__m256 v){return _mm256_min_ps(one,_mm256_max_ps(zero,v));};
        auto channel=[](__m256i p,int c){return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(p,_mm256_set1_epi32(c*8)),_mm256_set1_epi32(255)));};
        std::vector<std::array<float,3>> vertical(cols+1);
        const float fy=(float(y)+.5f)/grid-.5f;const int row=int(std::floor(fy));const float ty=fy-row;
        for(uint32_t i=0;i<cols;++i){const auto& a=tensor[size_t(std::clamp(row,0,int(rows)-1))*cols+i];const auto& b=tensor[size_t(std::clamp(row+1,0,int(rows)-1))*cols+i];vertical[i]={a.xx+(b.xx-a.xx)*ty,a.xy+(b.xy-a.xy)*ty,a.yy+(b.yy-a.yy)*ty};}vertical[cols]=vertical[cols-1];
        for(uint32_t x=0;x<width;){const int cell=x<grid/2?-1:int((x-grid/2)/grid);const auto lanes=std::min(8u,std::min(width-x,uint32_t((cell+1)*grid+grid/2)-x));const auto vx=_mm256_setr_ps(float(x),float(x+1),float(x+2),float(x+3),float(x+4),float(x+5),float(x+6),float(x+7)),vy=_mm256_set1_ps(float(y));
            const auto fraction=_mm256_mul_ps(_mm256_sub_ps(vx,_mm256_set1_ps(float(cell*int(grid)+int(grid)/2)-.5f)),_mm256_set1_ps(1.f/grid));const auto& a=vertical[std::clamp(cell,0,int(cols)-1)];const auto& b=vertical[std::clamp(cell+1,0,int(cols)-1)];auto field=[&](int c){return _mm256_add_ps(_mm256_set1_ps(a[c]),_mm256_mul_ps(fraction,_mm256_set1_ps(b[c]-a[c])));};
            alignas(32)float ax[8],ay[8],az[8];_mm256_store_ps(ax,field(0));_mm256_store_ps(ay,field(1));_mm256_store_ps(az,field(2));
            const auto xx=_mm256_load_ps(ax),xy=_mm256_load_ps(ay),yy=_mm256_load_ps(az),difference=_mm256_sub_ps(xx,yy);const auto disc=_mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(difference,difference),_mm256_mul_ps(_mm256_set1_ps(4),_mm256_mul_ps(xy,xy)))),coherence=sat(_mm256_div_ps(disc,_mm256_add_ps(_mm256_add_ps(xx,yy),_mm256_set1_ps(4))));
            const auto ratio=_mm256_div_ps(difference,_mm256_max_ps(disc,_mm256_set1_ps(.0001f))),nx=_mm256_sqrt_ps(sat(_mm256_mul_ps(half,_mm256_add_ps(one,ratio)))),ny=_mm256_xor_ps(_mm256_sqrt_ps(sat(_mm256_mul_ps(half,_mm256_sub_ps(one,ratio)))),_mm256_and_ps(xy,_mm256_set1_ps(-0.f)));
            auto fetch=[&](__m256 px,__m256 py){const auto xi=_mm256_cvttps_epi32(_mm256_min_ps(_mm256_set1_ps(float(width-1)),_mm256_max_ps(zero,px))),yi=_mm256_cvttps_epi32(_mm256_min_ps(_mm256_set1_ps(float(height-1)),_mm256_max_ps(zero,py)));return _mm256_i32gather_epi32(reinterpret_cast<const int*>(source.data()),_mm256_add_epi32(_mm256_mullo_epi32(yi,_mm256_set1_epi32(width)),xi),4);};
            const auto own=fetch(vx,vy);__m256 original[3]={channel(own,0),channel(own,1),channel(own,2)},sum[3]={original[0],original[1],original[2]};auto weights=one;auto valid=_mm256_cmpeq_epi32(_mm256_srli_epi32(own,24),_mm256_set1_epi32(255));
            // Four bilinear tangential samples; common RGB weights cannot
            // invent independent red/blue contour positions.
            for(float step:{-1.f,-.5f,.5f,1.f}){const auto distance=_mm256_set1_ps(step*radius),px=_mm256_sub_ps(vx,_mm256_mul_ps(ny,distance)),py=_mm256_add_ps(vy,_mm256_mul_ps(nx,distance)),fx=_mm256_floor_ps(px),fy=_mm256_floor_ps(py),tx=_mm256_sub_ps(px,fx),ty=_mm256_sub_ps(py,fy);const auto a=fetch(fx,fy),b=fetch(_mm256_add_ps(fx,one),fy),c=fetch(fx,_mm256_add_ps(fy,one)),d=fetch(_mm256_add_ps(fx,one),_mm256_add_ps(fy,one));valid=_mm256_and_si256(valid,_mm256_cmpeq_epi32(_mm256_srli_epi32(_mm256_and_si256(_mm256_and_si256(a,b),_mm256_and_si256(c,d)),24),_mm256_set1_epi32(255)));__m256 value[3];auto error=zero;
                for(int k=0;k<3;++k){const auto upper=_mm256_add_ps(channel(a,k),_mm256_mul_ps(tx,_mm256_sub_ps(channel(b,k),channel(a,k)))),lower=_mm256_add_ps(channel(c,k),_mm256_mul_ps(tx,_mm256_sub_ps(channel(d,k),channel(c,k))));value[k]=_mm256_add_ps(upper,_mm256_mul_ps(ty,_mm256_sub_ps(lower,upper)));error=_mm256_max_ps(error,abs(_mm256_sub_ps(value[k],original[k])));}
                auto v=sat(_mm256_sub_ps(one,_mm256_mul_ps(error,_mm256_set1_ps(1/range))));v=_mm256_mul_ps(_mm256_mul_ps(v,v),_mm256_set1_ps(std::abs(step)==1?.7f:1.f));weights=_mm256_add_ps(weights,v);for(int k=0;k<3;++k)sum[k]=_mm256_add_ps(sum[k],_mm256_mul_ps(v,value[k]));
            }
            // Preserve narrow strokes using opposing contrast, rather than
            // treating every high-frequency oscillation as protected detail.
            auto guard=zero;for(int axis=0;axis<2;++axis){const auto a=fetch(_mm256_sub_ps(vx,_mm256_set1_ps(axis?0.f:2.f)),_mm256_sub_ps(vy,_mm256_set1_ps(axis?2.f:0.f))),b=fetch(_mm256_add_ps(vx,_mm256_set1_ps(axis?0.f:2.f)),_mm256_add_ps(vy,_mm256_set1_ps(axis?2.f:0.f)));auto dot=zero,aa=one,bb=one,contrast=zero;for(int k=0;k<3;++k){const auto da=_mm256_sub_ps(original[k],channel(a,k)),db=_mm256_sub_ps(original[k],channel(b,k));dot=_mm256_add_ps(dot,_mm256_mul_ps(da,db));aa=_mm256_add_ps(aa,_mm256_mul_ps(da,da));bb=_mm256_add_ps(bb,_mm256_mul_ps(db,db));contrast=_mm256_max_ps(contrast,_mm256_min_ps(abs(da),abs(db)));}const auto correlation=_mm256_mul_ps(dot,_mm256_rsqrt_ps(_mm256_mul_ps(aa,bb)));guard=_mm256_max_ps(guard,_mm256_mul_ps(sat(_mm256_mul_ps(_mm256_sub_ps(correlation,half),_mm256_set1_ps(2))),sat(_mm256_mul_ps(_mm256_sub_ps(contrast,_mm256_set1_ps(12)),_mm256_set1_ps(.05f)))));}
            const auto amount=_mm256_mul_ps(_mm256_set1_ps(strength),_mm256_mul_ps(_mm256_sub_ps(one,guard),_mm256_add_ps(_mm256_set1_ps(.35f),_mm256_mul_ps(coherence,_mm256_set1_ps(.65f)))));__m256i output= _mm256_and_si256(own,_mm256_set1_epi32(int(0xff000000u)));for(int k=0;k<3;++k){const auto value=_mm256_add_ps(original[k],_mm256_mul_ps(amount,_mm256_sub_ps(_mm256_div_ps(sum[k],weights),original[k])));const auto rounded=_mm256_cvttps_epi32(_mm256_add_ps(half,_mm256_min_ps(_mm256_set1_ps(255),_mm256_max_ps(zero,value))));output=_mm256_or_si256(output,_mm256_sllv_epi32(rounded,_mm256_set1_epi32(k*8)));}output=_mm256_blendv_epi8(own,output,valid);alignas(32)uint8_t bytes[32];_mm256_store_si256(reinterpret_cast<__m256i*>(bytes),output);std::memcpy(pixels.data()+(size_t(y)*width+x)*4,bytes,lanes*4);x+=lanes;
        }
    });_mm256_zeroupper();
}
void compactTangentCleanup(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,float strength,uint32_t scale,float trim,float stronger){
    if(pixels.size()!=pixelBytes(width,height)||!std::isfinite(strength)||strength<0||strength>1||(scale!=2&&scale!=4&&scale!=8)||!std::isfinite(trim)||trim< -1||trim>.2f||!std::isfinite(stronger)||stronger<0||stronger>1)throw std::runtime_error("Invalid compact tangent controls.");if(!strength)return;
    // Correction is estimated at reduced resolution. The full-resolution
    // residual stays in place; only small inconsistent components are reduced.
    const uint32_t w=(width+scale-1)/scale,h=(height+scale-1)/scale;Bytes low(pixelBytes(w,h),255);
    parallelFor(h,workers,[&](uint32_t y){for(uint32_t x=0;x<w;++x){uint32_t sum[3]{},count=0;bool opaque=true;for(uint32_t yy=y*scale;yy<std::min(height,(y+1)*scale);++yy)for(uint32_t xx=x*scale;xx<std::min(width,(x+1)*scale);++xx){const auto* p=pixels.data()+(size_t(yy)*width+xx)*4;for(int c=0;c<3;++c)sum[c]+=p[c];opaque&=p[3]==255;++count;}auto* p=low.data()+(size_t(y)*w+x)*4;for(int c=0;c<3;++c)p[c]=uint8_t((sum[c]+count/2)/count);p[3]=opaque?255:0;}});
    auto clean=low;
    if(trim>=0){coherent441::Image input(size_t(w)*h);for(size_t i=0;i<input.size();++i)for(int c=0;c<3;++c)input[i][c]=low[i*4+c];
        const auto corrected=coherent441::monotone(input,w,h,workers,.65f,trim,1.f/scale);
        for(size_t i=0;i<input.size();++i)for(int c=0;c<3;++c)clean[i*4+c]=uint8_t(std::clamp(int(corrected[i][c]+.5f),0,255));}
    tangentCleanup(clean,w,h,workers,1,std::max(1.f,5.f/scale),20+stronger*36);tangentCleanup(clean,w,h,workers,1,std::max(1.f,5.f/scale),20+stronger*36);
    parallelFor(height,workers,[&](uint32_t y){const auto zero=_mm256_setzero_ps(),one=_mm256_set1_ps(1),half=_mm256_set1_ps(.5f),absMask=_mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));auto abs=[&](__m256 v){return _mm256_and_ps(v,absMask);};auto sat=[&](__m256 v){return _mm256_min_ps(one,_mm256_max_ps(zero,v));};const float fy=(float(y)+.5f)/scale-.5f;const int iy=int(std::floor(fy));const float ty=fy-iy;
        struct alignas(16) Value{float base[4]{},delta[4]{};};std::vector<Value> row(w+1);std::vector<uint8_t> opaque(w+1);
        for(uint32_t x=0;x<w;++x){const auto at=(size_t(std::clamp(iy,0,int(h)-1))*w+x)*4,bt=(size_t(std::clamp(iy+1,0,int(h)-1))*w+x)*4;for(int c=0;c<3;++c){row[x].base[c]=low[at+c]*(1-ty)+low[bt+c]*ty;row[x].delta[c]=(int(clean[at+c])-low[at+c])*(1-ty)+(int(clean[bt+c])-low[bt+c])*ty;}opaque[x]=low[at+3]==255&&low[bt+3]==255;}row[w]=row[w-1];opaque[w]=opaque[w-1];
        for(uint32_t x=0;x<width;){const int cell=x<scale/2?-1:int((x-scale/2)/scale);const uint32_t lanes=std::min(8u,std::min(width-x,uint32_t((cell+1)*int(scale)+int(scale)/2)-x));const auto& a=row[std::clamp(cell,0,int(w)-1)];const auto& b=row[std::clamp(cell+1,0,int(w)-1)];const auto fraction=_mm256_setr_ps((x-float(cell*int(scale)+int(scale)/2)+.5f)/scale,(x+1-float(cell*int(scale)+int(scale)/2)+.5f)/scale,(x+2-float(cell*int(scale)+int(scale)/2)+.5f)/scale,(x+3-float(cell*int(scale)+int(scale)/2)+.5f)/scale,(x+4-float(cell*int(scale)+int(scale)/2)+.5f)/scale,(x+5-float(cell*int(scale)+int(scale)/2)+.5f)/scale,(x+6-float(cell*int(scale)+int(scale)/2)+.5f)/scale,(x+7-float(cell*int(scale)+int(scale)/2)+.5f)/scale);
            alignas(32)uint8_t bytes[32]{};auto* p=pixels.data()+(size_t(y)*width+x)*4;std::memcpy(bytes,p,lanes*4);const auto packed=_mm256_load_si256(reinterpret_cast<const __m256i*>(bytes));__m256 own[3],base[3],delta[3];auto error=zero;
            for(int c=0;c<3;++c){own[c]=_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(packed,_mm256_set1_epi32(c*8)),_mm256_set1_epi32(255)));base[c]=_mm256_add_ps(_mm256_set1_ps(a.base[c]),_mm256_mul_ps(fraction,_mm256_set1_ps(b.base[c]-a.base[c])));delta[c]=_mm256_add_ps(_mm256_set1_ps(a.delta[c]),_mm256_mul_ps(fraction,_mm256_set1_ps(b.delta[c]-a.delta[c])));error=_mm256_max_ps(error,abs(_mm256_sub_ps(own[c],base[c])));}
            const auto denoise=_mm256_mul_ps(_mm256_set1_ps(.65f),sat(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(24),error),_mm256_set1_ps(1.f/24))));auto output=_mm256_and_si256(packed,_mm256_set1_epi32(int(0xff000000u)));
            for(int c=0;c<3;++c){const auto correction=_mm256_add_ps(delta[c],_mm256_mul_ps(denoise,_mm256_sub_ps(base[c],own[c])));const auto value=_mm256_add_ps(own[c],_mm256_mul_ps(_mm256_set1_ps(strength),correction));const auto rounded=_mm256_cvttps_epi32(_mm256_add_ps(half,_mm256_min_ps(_mm256_set1_ps(255),_mm256_max_ps(zero,value))));output=_mm256_or_si256(output,_mm256_sllv_epi32(rounded,_mm256_set1_epi32(c*8)));}
            auto valid=_mm256_cmpeq_epi32(_mm256_srli_epi32(packed,24),_mm256_set1_epi32(255));if(!opaque[std::clamp(cell,0,int(w)-1)]||!opaque[std::clamp(cell+1,0,int(w)-1)])valid=_mm256_setzero_si256();output=_mm256_blendv_epi8(packed,output,valid);_mm256_store_si256(reinterpret_cast<__m256i*>(bytes),output);std::memcpy(p,bytes,lanes*4);x+=lanes;
        }
    });_mm256_zeroupper();
}
}
