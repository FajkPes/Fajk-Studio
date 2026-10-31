#pragma once
#include "fajk/hardware.hpp"
#include <array>
#include <cmath>
#include <immintrin.h>
#include "timing.hpp"
namespace fajk {void coherentBoxBlur(const float*,float*,float*,uint32_t,uint32_t,int,uint32_t);bool hasAvx2();}
namespace fajk {void coherentTangentPass(const float*,float*,const float*,const float*,uint32_t,uint32_t,uint32_t,float,float,float);}
namespace fajk::coherent441 {
using Color=std::array<float,3>;
using Image=std::vector<Color>;
inline Image blur(const Image& image,uint32_t w,uint32_t h,int radius,uint32_t workers){
    Image temp(image.size()),out(image.size());
    if(hasAvx2()){coherentBoxBlur(image[0].data(),temp[0].data(),out[0].data(),w,h,radius,workers);return out;}
    parallelFor(h,workers,[&](uint32_t y){Color sum{};for(int x=-radius;x<=radius;++x)for(int c=0;c<3;++c)sum[c]+=image[size_t(y)*w+std::clamp(x,0,int(w)-1)][c];
        for(uint32_t x=0;x<w;++x){for(int c=0;c<3;++c)temp[size_t(y)*w+x][c]=sum[c]/(radius*2+1);const int left=std::clamp(int(x)-radius,0,int(w)-1),right=std::clamp(int(x)+radius+1,0,int(w)-1);for(int c=0;c<3;++c)sum[c]+=image[size_t(y)*w+right][c]-image[size_t(y)*w+left][c];}});
    parallelFor(w,workers,[&](uint32_t x){Color sum{};for(int y=-radius;y<=radius;++y)for(int c=0;c<3;++c)sum[c]+=temp[size_t(std::clamp(y,0,int(h)-1))*w+x][c];
        for(uint32_t y=0;y<h;++y){for(int c=0;c<3;++c)out[size_t(y)*w+x][c]=sum[c]/(radius*2+1);const int top=std::clamp(int(y)-radius,0,int(h)-1),bottom=std::clamp(int(y)+radius+1,0,int(h)-1);for(int c=0;c<3;++c)sum[c]+=temp[size_t(bottom)*w+x][c]-temp[size_t(top)*w+x][c];}});return out;
}
inline Color sample(const Image& image,uint32_t w,uint32_t h,float x,float y){
    x=std::clamp(x,0.f,float(w-1));y=std::clamp(y,0.f,float(h-1));const int ax=int(x),ay=int(y),bx=std::min(ax+1,int(w)-1),by=std::min(ay+1,int(h)-1);const float tx=x-ax,ty=y-ay;Color result{};
    for(int c=0;c<3;++c)result[c]=(image[size_t(ay)*w+ax][c]*(1-tx)+image[size_t(ay)*w+bx][c]*tx)*(1-ty)+(image[size_t(by)*w+ax][c]*(1-tx)+image[size_t(by)*w+bx][c]*tx)*ty;return result;
}
inline Color point(const Image& image,uint32_t w,uint32_t h,int x,int y){return image[size_t(std::clamp(y,0,int(h)-1))*w+std::clamp(x,0,int(w)-1)];}
inline Image coherent(const Image& input,uint32_t w,uint32_t h,uint32_t workers,float radius,float range,int iterations,int protection=0,float sharpen=0,float stronger=0){
    TimingTimeline timing;
    // A dense RGB structure tensor supplies a common direction for every
    // color. Tangential samples do not need a successful two-color edge fit.
    const auto guide=blur(blur(input,w,h,2,workers),w,h,2,workers);Image tensor(input.size());timing.mark("decode.cpu441.guide");
    parallelFor(h,workers,[&](uint32_t y){for(uint32_t x=0;x<w;++x){const auto a=point(guide,w,h,int(x)-1,y),b=point(guide,w,h,int(x)+1,y),c=point(guide,w,h,x,int(y)-1),d=point(guide,w,h,x,int(y)+1);Color t{};for(int k=0;k<3;++k){const float gx=(b[k]-a[k])*.5f,gy=(d[k]-c[k])*.5f;t[0]+=gx*gx;t[1]+=gx*gy;t[2]+=gy*gy;}tensor[size_t(y)*w+x]=t;}});
    tensor=blur(tensor,w,h,2,workers);Image directions(input.size());timing.mark("decode.cpu441.tensor");
    parallelFor(h,workers,[&](uint32_t y){for(uint32_t x=0;x<w;++x){const auto t=tensor[size_t(y)*w+x];const float angle=.5f*std::atan2(2*t[1],t[0]-t[2]),difference=std::hypot(t[0]-t[2],2*t[1]);directions[size_t(y)*w+x]={-std::sin(angle),std::cos(angle),difference/(t[0]+t[2]+4)};}});
    timing.mark("decode.cpu441.directions");std::vector<float> detail(input.size(),0);
    if(protection)parallelFor(h,workers,[&](uint32_t y){for(uint32_t x=0;x<w;++x){const auto at=size_t(y)*w+x;float guard=0;
        // A narrow real stroke returns toward the same surrounding color on
        // both sides. A single silhouette does not. Use the decoded original,
        // so successive cleanup passes cannot erase their own protection.
        for(const auto axis:std::array<std::array<int,2>,4>{{{1,0},{0,1},{1,1},{1,-1}}})for(int distance:{2,4}){const auto a=point(input,w,h,int(x)-axis[0]*distance,int(y)-axis[1]*distance),b=point(input,w,h,int(x)+axis[0]*distance,int(y)+axis[1]*distance);float dot=0,aa=0,bb=0,contrast=0;
            for(int c=0;c<3;++c){const float da=input[at][c]-a[c],db=input[at][c]-b[c];dot+=da*db;aa+=da*da;bb+=db*db;contrast=std::max(contrast,std::min(std::abs(da),std::abs(db)));}
            const float correlation=dot/std::sqrt(aa*bb+1);guard=std::max(guard,std::clamp((correlation-.5f)*2,0.f,1.f)*std::clamp((contrast-(protection>=2?12.f:20.f))/20,0.f,1.f));
        }detail[at]=guard;}});
    timing.mark("decode.cpu441.protection");Image current=input,next(input.size());
    for(int iteration=0;iteration<iterations;++iteration){
        if(protection==1&&hasAvx2()){coherentTangentPass(current[0].data(),next[0].data(),directions[0].data(),detail.data(),w,h,workers,radius,range,stronger);current.swap(next);timing.mark("decode.cpu441.iteration");continue;}
        parallelFor(h,workers,[&](uint32_t y){for(uint32_t x=0;x<w;++x){const auto at=size_t(y)*w+x;const auto own=current[at],dir=directions[at];Color sum=own;float weight=1;
        const float flow=protection==4?1.f:stronger;
        for(int axis=0;axis<2;++axis)for(int step=-3;step<=3;++step){if(!step)continue;const float along=step*radius/3,dx=axis?-dir[1]:dir[0],dy=axis?dir[0]:dir[1],distance=axis?along*(1-.85f*dir[2]):along;const auto p=sample(current,w,h,x+dx*distance,y+dy*distance);float error=0;for(int c=0;c<3;++c)error=std::max(error,std::abs(p[c]-own[c]));const float localRange=range*(protection==3?.4f+.6f*dir[2]:1.f)+(!axis?flow*36*dir[2]:0),r=error/localRange;float v=std::max(0.f,1-r*r);v*=v*(step*step==9?.35f:step*step==4?.7f:1.f);if(flow){v*=1-detail[at]*(axis?1.f:1-dir[2]);if(axis)v*=1-flow*dir[2];}for(int c=0;c<3;++c)sum[c]+=v*p[c];weight+=v;}
        const float coherence=protection==3?.3f+.7f*dir[2]:1.f;for(int c=0;c<3;++c)next[at][c]=own[c]+.65f*coherence*(1-detail[at]*(1-flow))*(sum[c]/weight-own[c]);}});current.swap(next);
    }
    if(sharpen){next=current;parallelFor(h,workers,[&](uint32_t y){for(uint32_t x=0;x<w;++x){const auto at=size_t(y)*w+x;const auto dir=directions[at];if(dir[2]<.6f||detail[at]>.5f)continue;const auto a=sample(current,w,h,x-dir[1]*1.5f,y+dir[0]*1.5f),b=sample(current,w,h,x+dir[1]*1.5f,y-dir[0]*1.5f);float energy=1,t=0;Color delta{};for(int c=0;c<3;++c){delta[c]=b[c]-a[c];energy+=delta[c]*delta[c];t+=(current[at][c]-a[c])*delta[c];}t=std::clamp(t/energy,0.f,1.f);if(energy<144)continue;const float target=t*t*(3-2*t),change=sharpen*(target-t);for(int c=0;c<3;++c)next[at][c]=std::clamp(current[at][c]+change*delta[c],std::min(a[c],b[c]),std::max(a[c],b[c]));}});current.swap(next);}
    return current;
}
inline Image rolling(const Image& input,uint32_t w,uint32_t h,uint32_t workers,int radius,float range){
    auto guide=blur(blur(input,w,h,radius/2,workers),w,h,radius/2,workers);Image next(input.size());
    for(int iteration=0;iteration<4;++iteration){parallelFor(h,workers,[&](uint32_t y){for(uint32_t x=0;x<w;++x){const auto at=size_t(y)*w+x;Color sum{};float weight=0;
        for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx){const auto other=size_t(std::clamp(int(y)+dy,0,int(h)-1))*w+std::clamp(int(x)+dx,0,int(w)-1);float error=0;for(int c=0;c<3;++c){const float d=guide[at][c]-guide[other][c];error+=d*d;}const float spatial=std::max(0.f,1-float(dx*dx+dy*dy)/float((radius+1)*(radius+1))),r=error/(range*range*3);float v=std::max(0.f,1-r);v*=v*spatial;weight+=v;for(int c=0;c<3;++c)sum[c]+=v*input[other][c];}
        for(int c=0;c<3;++c)next[at][c]=sum[c]/weight;
    }});guide.swap(next);}return guide;
}
inline Image monotone(const Image& input,uint32_t w,uint32_t h,uint32_t workers,float amount,float trim=0,float spatialScale=1){
    const int guideRadius=std::max(1,int(2*spatialScale+.5f));
    const auto guide=blur(blur(input,w,h,guideRadius,workers),w,h,guideRadius,workers);Image out=input;
    parallelFor(h,workers,[&](uint32_t y){for(uint32_t x=0;x<w;++x){const auto at=size_t(y)*w+x;const auto left=point(guide,w,h,int(x)-2,y),right=point(guide,w,h,int(x)+2,y),top=point(guide,w,h,x,int(y)-2),bottom=point(guide,w,h,x,int(y)+2);float xx=0,xy=0,yy=0;
        for(int c=0;c<3;++c){const float gx=right[c]-left[c],gy=bottom[c]-top[c];xx+=gx*gx;xy+=gx*gy;yy+=gy*gy;}const float angle=.5f*std::atan2(2*xy,xx-yy),nx=std::cos(angle)*spatialScale,ny=std::sin(angle)*spatialScale;std::array<Color,17> profile;profile[0]=sample(input,w,h,x-8*nx,y-8*ny);profile[16]=sample(input,w,h,x+8*nx,y+8*ny);
        Color delta{};float energy=1;for(int c=0;c<3;++c){delta[c]=profile[16][c]-profile[0][c];energy+=delta[c]*delta[c];}if(energy<2400)continue;
        for(int i=1;i<16;++i)profile[i]=sample(input,w,h,x+(i-8)*nx,y+(i-8)*ny);
        float phase[17],variation=0;for(int i=0;i<17;++i){float dot=0;for(int c=0;c<3;++c)dot+=(profile[i][c]-profile[0][c])*delta[c];phase[i]=dot/energy;if(i)variation+=std::abs(phase[i]-phase[i-1]);}
        // Repeated deep crossings are a stroke/texture, not a ringing skirt.
        if(variation>1.8f)continue;
        float mean[17],weight[17];int first[17],last[17],blocks=0;
        for(int i=0;i<17;++i){mean[blocks]=std::clamp(phase[i],0.f,1.f);weight[blocks]=(i==0||i==16)?4.f:1.f;first[blocks]=last[blocks]=i;++blocks;
            while(blocks>1&&mean[blocks-2]>mean[blocks-1]){const auto b=blocks-2;mean[b]=(mean[b]*weight[b]+mean[b+1]*weight[b+1])/(weight[b]+weight[b+1]);weight[b]+=weight[b+1];last[b]=last[b+1];--blocks;}}
        float target=phase[8];for(int b=0;b<blocks;++b)if(first[b]<=8&&last[b]>=8)target=mean[b];
        if(trim){float error=0;for(int c=0;c<3;++c)error=std::max(error,std::abs(input[at][c]-(profile[0][c]+phase[8]*delta[c])));const float reliable=std::clamp((25-error)/15,0.f,1.f);const float narrowed=std::clamp((target-trim)/(1-2*trim),0.f,1.f);target+=reliable*(narrowed-target);}
        const float correction=amount*(target-phase[8]);
        for(int c=0;c<3;++c){const float value=input[at][c]+std::clamp(correction*delta[c],-18.f,18.f);out[at][c]=std::clamp(value,std::min({input[at][c],profile[0][c],profile[16][c]}),std::max({input[at][c],profile[0][c],profile[16][c]}));}
    }});return out;
}
}
