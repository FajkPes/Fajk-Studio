#pragma once
#include "fajk/types.hpp"
#include "fajk/hardware.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
namespace fajk {
// Encoder-only adaptive color resolution: omit the two finest chroma bands
// only outside source-confirmed color transitions. Ordinary zero groups carry
// the selection, so the decoder has no mask, search or new stream syntax.
inline void omitSmoothChromaDetails(const Frame& f,std::array<std::vector<float>,3>& planes,int stride,int height,float threshold,uint32_t threads){
    require(std::isfinite(threshold)&&threshold>0&&threshold<=64,"Invalid adaptive chroma threshold.");
    require(stride>=int(f.width)&&height>=int(f.height)&&planes[1].size()==size_t(stride)*height&&planes[2].size()==planes[1].size(),"Invalid adaptive chroma planes.");
    const int cols=(int(f.width)+7)/8,rows=(int(f.height)+7)/8;
    std::vector<uint32_t> edge(size_t(cols)*rows),sum(size_t(cols+1)*(rows+1));
    auto difference=[](const uint8_t* a,const uint8_t* b){const int dg=int(a[1])-b[1];return std::max(std::abs(int(a[0])-b[0]-dg),std::abs(int(a[2])-b[2]-dg));};
    parallelFor(uint32_t(rows),threads,[&](uint32_t row){for(int x=0;x<int(f.width);++x)for(int y=int(row)*8;y<std::min(int(f.height),int(row)*8+8);++y){const auto* a=f.pixels.data()+(size_t(y)*f.width+x)*4;const bool boundary=(x&&difference(a,a-4)>=threshold)||(y&&difference(a,a-size_t(f.width)*4)>=threshold);if(boundary)edge[size_t(row)*cols+x/8]=1;}});
    for(int y=0;y<rows;++y){uint32_t total=0;for(int x=0;x<cols;++x){total+=edge[size_t(y)*cols+x];sum[size_t(y+1)*(cols+1)+x+1]=sum[size_t(y)*(cols+1)+x+1]+total;}}
    auto occupied=[&](int x,int y,int w,int h,int scale){const int halo=8*scale,x0=std::clamp((x*scale-halo)/8,0,cols),y0=std::clamp((y*scale-halo)/8,0,rows),x1=std::clamp(((x+w)*scale+halo+7)/8,0,cols),y1=std::clamp(((y+h)*scale+halo+7)/8,0,rows);return sum[size_t(y1)*(cols+1)+x1]+sum[size_t(y0)*(cols+1)+x0]!=sum[size_t(y1)*(cols+1)+x0]+sum[size_t(y0)*(cols+1)+x1];};
    parallelFor(12,threads,[&](uint32_t job){const int c=1+job/6,level=(job%6)/3,band=job%3,bw=stride>>(level+1),bh=height>>(level+1),bx=band==1?0:bw,by=band==0?0:bh,scale=1<<(level+1);for(int y=0;y<bh;y+=4)for(int x=0;x<bw;x+=4){const int w=std::min(4,bw-x),h=std::min(4,bh-y);if(occupied(x,y,w,h,scale))continue;for(int dy=0;dy<h;++dy)std::fill_n(planes[c].data()+size_t(by+y+dy)*stride+bx+x,w,0.f);}});
}
}
