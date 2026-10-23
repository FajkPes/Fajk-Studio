#pragma once
#include "fajk/hardware.hpp"
#include <cmath>
#include <vector>

namespace fajk {
// Encoder-only contrast-normalized pixel loss. Neither the image resolution
// nor reconstruction filters change. The actual synthesis adjoint projects
// the weighted pilot residual into coefficient space.
struct ContrastLoss {
    int width=0,height=0;
    std::vector<float> weights;
    std::vector<double> integral;
    float basisWeight(int x,int y,int radius) const {
        const int left=std::max(0,x-radius),right=std::min(width,x+radius+1);
        const int top=std::max(0,y-radius),bottom=std::min(height,y+radius+1);
        const size_t stride=size_t(width)+1;
        const double sum=integral[size_t(bottom)*stride+right]-integral[size_t(top)*stride+right]
                        -integral[size_t(bottom)*stride+left]+integral[size_t(top)*stride+left];
        return float(sum/((right-left)*(bottom-top)));
    }
};
inline ContrastLoss fitContrastLoss(const std::vector<float>& source,int w,int h,uint32_t threads,uint8_t mode){
    require(w>0&&h>0&&source.size()==size_t(w)*h&&mode<3,"Invalid contrast-loss source.");
    const int cols=(w+7)/8,rows=(h+7)/8;
    std::vector<float> grid(size_t(cols)*rows);
    constexpr float exponents[3]={.25f,.5f,1.f};
    parallelFor(uint32_t(rows),threads,[&](uint32_t ty){for(int tx=0;tx<cols;++tx){
        double sum=0,square=0;int count=0;
        for(int y=int(ty)*8;y<std::min(h,int(ty+1)*8);++y)for(int x=tx*8;x<std::min(w,(tx+1)*8);++x){
            const float v=source[size_t(y)*w+x];sum+=v;square+=double(v)*v;++count;
        }
        const double mean=sum/count,variance=std::max(0.,square/count-mean*mean);
        grid[size_t(ty)*cols+tx]=std::clamp(std::pow(16.f/(8.f+float(std::sqrt(variance))),exponents[mode]),.25f,2.f);
    }});
    double total=0;for(auto value:grid)total+=value;
    const float normalization=float(grid.size()/total);
    ContrastLoss result;result.width=w;result.height=h;result.weights.resize(source.size());
    parallelFor(uint32_t(h),threads,[&](uint32_t y){
        const float gy=(float(y)+.5f)/8-.5f;const int ay=int(std::floor(gy)),by=ay+1;const float fy=gy-ay;
        for(int x=0;x<w;++x){const float gx=(x+.5f)/8-.5f;const int ax=int(std::floor(gx)),bx=ax+1;const float fx=gx-ax;
            auto at=[&](int xx,int yy){return grid[size_t(std::clamp(yy,0,rows-1))*cols+std::clamp(xx,0,cols-1)];};
            result.weights[size_t(y)*w+x]=normalization*((1-fy)*((1-fx)*at(ax,ay)+fx*at(bx,ay))+fy*((1-fx)*at(ax,by)+fx*at(bx,by)));
        }
    });
    const size_t stride=size_t(w)+1;result.integral.assign(stride*(size_t(h)+1),0);
    for(int y=0;y<h;++y){double sum=0;for(int x=0;x<w;++x){sum+=result.weights[size_t(y)*w+x];result.integral[size_t(y+1)*stride+x+1]=result.integral[size_t(y)*stride+x+1]+sum;}}
    return result;
}
}
