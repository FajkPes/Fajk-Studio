#pragma once
#include "fajk/hardware.hpp"
#include <array>
namespace fajk {
bool hasAvx2();
uint32_t colorBoundaryRowAvx2(const uint8_t*,uint8_t*,uint32_t,uint32_t,uint32_t);
// Narrow a transition only when its samples fit a segment between two locally
// flat colors. Positive interpolation never creates halos or new RGB extrema.
inline Frame restoreColorBoundaries(const Frame& source,uint32_t threads,float amount,float slope=1.f,int radius=2,float colorProjection=0,bool continuous=false,bool optimized=true,std::span<uint8_t> destination={},uint32_t firstRow=0,uint32_t endRow=0){
    source.validate();require(amount>=0&&amount<=1&&std::isfinite(slope)&&slope>=1&&slope<=3&&(radius==2||radius==4||radius==6)&&colorProjection>=0&&colorProjection<=1,"Invalid color boundary strength.");require(destination.empty()||destination.size()==source.pixels.size(),"Invalid color boundary destination.");if(!endRow)endRow=source.height;require(firstRow<=endRow&&endRow<=source.height,"Invalid color boundary rows.");Frame result{source.width,source.height,source.pts,source.duration,destination.empty()?source.pixels:Bytes{}};const auto output=destination.empty()?std::span(result.pixels):destination;if(!destination.empty())std::copy(source.pixels.begin()+size_t(firstRow)*source.width*4,source.pixels.begin()+size_t(endRow)*source.width*4,output.begin()+size_t(firstRow)*source.width*4);
    const int margin=radius+radius/2;
    if(amount==0||source.width<uint32_t(2*margin+1)||source.height<uint32_t(2*margin+1))return result;
    using Color=std::array<float,3>;constexpr std::array<std::array<int,2>,4> normals{{{1,0},{0,1},{1,1},{1,-1}}};
    auto load=[&](int x,int y){const auto* p=source.pixels.data()+(size_t(y)*source.width+x)*4;return Color{float(p[0]),float(p[1]),float(p[2])};};
    auto square=[](const Color& a,const Color& b){float s=0;for(int c=0;c<3;++c){const float d=a[c]-b[c];s+=d*d;}return s;};
    const bool vectorized=optimized&&amount==1.f&&slope==3.f&&radius==2&&colorProjection==0&&!continuous&&hasAvx2();
    const auto begin=std::max(firstRow,uint32_t(margin)),end=std::min(endRow,source.height-uint32_t(margin));if(begin>=end)return result;
    parallelFor(end-begin,threads,[&](uint32_t row){const int y=int(row+begin);
      const int first=vectorized?int(colorBoundaryRowAvx2(source.pixels.data(),output.data(),source.width,uint32_t(y),uint32_t(margin))):margin;
      for(int x=first;x<int(source.width)-margin;++x){
        // A locally constant pixel cannot benefit from narrowing a transition.
        // Skip it before loading all the distant samples of the edge detector.
        if(slope>1.f){
            const auto center=load(x,y);
            if(square(center,load(x-1,y))<9&&square(center,load(x+1,y))<9&&square(center,load(x,y-1))<9&&square(center,load(x,y+1))<9)continue;
        }
        Color low{},high{};float dx=1,dy=0,best=0;
        if(continuous){
            Color gx{},gy{};
            for(int offset=-1;offset<=1;++offset){const float weight=offset==0?2.f:1.f;
                const auto left=load(x-1,y+offset),right=load(x+1,y+offset),up=load(x+offset,y-1),down=load(x+offset,y+1);
                for(unsigned c=0;c<3;++c){gx[c]+=weight*(right[c]-left[c]);gy[c]+=weight*(down[c]-up[c]);}}
            float xx=0,yy=0,xy=0;for(unsigned c=0;c<3;++c){xx+=gx[c]*gx[c];yy+=gy[c]*gy[c];xy+=gx[c]*gy[c];}
            const float energy=xx+yy,disc=std::sqrt((xx-yy)*(xx-yy)+4*xy*xy);
            if(energy<57600||disc<energy*.8f)continue;
            // Stable principal eigenvector, avoiding atan/sin/cos per pixel.
            if(xx>=yy){dx=xx-yy+disc;dy=2*xy;}else{dx=2*xy;dy=yy-xx+disc;}
            const float inverse=1/std::sqrt(dx*dx+dy*dy);dx*=inverse;dy*=inverse;
        }else for(const auto& n:normals){const auto a=load(x-radius*n[0],y-radius*n[1]),b=load(x+radius*n[0],y+radius*n[1]);const float gradient=square(a,b)/float(n[0]&&n[1]?2:1);
            if(gradient>best){best=gradient;low=a;high=b;dx=float(n[0]);dy=float(n[1]);}}
        auto along=[&](float distance){if(!continuous)return load(x+int(distance*dx),y+int(distance*dy));
            const float px=std::clamp(x+distance*dx,0.f,float(source.width-1)),py=std::clamp(y+distance*dy,0.f,float(source.height-1));const int ix=std::clamp(int(std::floor(px)),0,int(source.width)-2),iy=std::clamp(int(std::floor(py)),0,int(source.height)-2);const float fx=px-ix,fy=py-iy;
            const auto a=load(ix,iy),b=load(ix+1,iy),c=load(ix,iy+1),d=load(ix+1,iy+1);Color result{};
            for(unsigned channel=0;channel<3;++channel)result[channel]=(a[channel]+fx*(b[channel]-a[channel]))*(1-fy)+(c[channel]+fx*(d[channel]-c[channel]))*fy;return result;};
        if(continuous){low=along(float(-radius));high=along(float(radius));}
        const float energy=square(low,high);if(energy<=900)continue;
        const float sideError=std::max(square(low,along(float(-margin))),square(high,along(float(margin))));
        const float flatConfidence=std::clamp(1-sideError/(energy*.06f+1),0.f,1.f);if(flatConfidence==0)continue;
        const auto center=load(x,y);Color difference{};for(int c=0;c<3;++c)difference[c]=high[c]-low[c];
        auto projection=[&](const Color& p){float dot=0;for(int c=0;c<3;++c)dot+=(p[c]-low[c])*difference[c];return dot/energy;};
        const float t=projection(center);if(colorProjection? t<-.05f||t>1.05f:t<=.02f||t>=.98f)continue;
        float residual=0;for(const auto& sample:{along(-1.f),center,along(1.f)}){const float at=std::clamp(projection(sample),0.f,1.f);Color predicted{};for(int c=0;c<3;++c)predicted[c]=low[c]+at*difference[c];residual=std::max(residual,square(predicted,sample));}
        const float lineConfidence=std::clamp(1-residual/(energy*(colorProjection?.1f:radius==2?.02f:.08f)+4),0.f,1.f);
        const float contrastConfidence=std::clamp((std::sqrt(energy)-30)/60,0.f,1.f);
        // Compress the transition around its midpoint without changing either
        // endpoint. Confidence gates protect smooth gradients and multicolor detail.
        const float target=slope==1.f?t*t*(3-2*t):std::clamp((t-.5f)*slope+.5f,0.f,1.f);
        const float shift=(target-t)*amount*flatConfidence*lineConfidence*contrastConfidence;
        if(shift==0&&colorProjection==0)continue;
        auto* out=output.data()+(size_t(y)*source.width+x)*4;
        const float projectionWeight=colorProjection*amount*flatConfidence*lineConfidence*contrastConfidence;
        for(int c=0;c<3;++c){const float offSegment=center[c]-(low[c]+std::clamp(t,0.f,1.f)*difference[c]);out[c]=uint8_t(std::clamp(int(std::clamp(center[c]+shift*difference[c]-projectionWeight*offSegment,std::min({center[c],low[c],high[c]}),std::max({center[c],low[c],high[c]}))+.5f),0,255));}
    }});return result;
}
}
