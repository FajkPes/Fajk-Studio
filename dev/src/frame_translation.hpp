#pragma once
#include "fajk/types.hpp"
namespace fajk {
struct FrameTranslation {int x=0,y=0;double error=0;};
// Diagnostic global camera estimate. Integer offsets intentionally avoid a
// resampling pass: the encoder grid follows the source instead of warping it.
inline FrameTranslation estimateFrameTranslation(const Frame& frame,const Frame& previous){
    frame.validate();previous.validate();require(frame.width==previous.width&&frame.height==previous.height,"Translation dimensions differ.");
    auto luma=[](const Frame& f,int x,int y){const auto at=(size_t(std::clamp(y,0,int(f.height)-1))*f.width+std::clamp(x,0,int(f.width)-1))*4;return (int(f.pixels[at])+2*int(f.pixels[at+1])+int(f.pixels[at+2])+2)/4;};
    uint32_t samples=0;
    auto cost=[&](int dx,int dy){int64_t sum=0,squared=0;uint32_t count=0;for(uint32_t y=16;y<frame.height;y+=32)for(uint32_t x=16;x<frame.width;x+=32){const int d=luma(frame,x,y)-luma(previous,int(x)+dx,int(y)+dy);sum+=d;squared+=int64_t(d)*d;++count;}samples=count;return count?squared-sum*sum/count:0;};
    FrameTranslation result;auto best=cost(0,0);
    for(int step:{32,16,8,4,2,1}){const int ox=result.x,oy=result.y;for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){const int dx=ox+x*step,dy=oy+y*step;const auto value=cost(dx,dy);if(value<best){best=value;result.x=dx;result.y=dy;}}}
    result.error=samples?std::sqrt(double(best)/samples):0;return result;
}
}
