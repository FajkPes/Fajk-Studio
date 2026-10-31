#pragma once
#include "fajk/types.hpp"
#include <array>

namespace fajk {
// Orthonormal 8x8 residual transform. Frequency planes are contiguous and have
// independent entropy models; no interpolation crosses a prediction boundary.
inline void temporalBlockTransform(std::vector<float>& plane,uint32_t width,uint32_t height,uint32_t workers,bool inverse) {
    require(width%8==0&&height%8==0&&plane.size()==size_t(width)*height,"Invalid residual block geometry.");
    static const auto basis=[] {
        std::array<std::array<float,8>,8> result{};
        for(unsigned k=0;k<8;++k)for(unsigned x=0;x<8;++x)
            result[k][x]=float((k?0.5:std::sqrt(0.125))*std::cos((2*x+1)*k*3.14159265358979323846/16));
        return result;
    }();
    const auto cols=width/8,rows=height/8;const size_t area=size_t(cols)*rows;
    std::vector<float> result(plane.size());
    parallelFor(rows,workers,[&](uint32_t by){for(uint32_t bx=0;bx<cols;++bx){
        float temporary[8][8]{};
        if(!inverse){
            for(unsigned y=0;y<8;++y)for(unsigned k=0;k<8;++k)for(unsigned x=0;x<8;++x)
                temporary[y][k]+=plane[size_t(by*8+y)*width+bx*8+x]*basis[k][x];
            for(unsigned ky=0;ky<8;++ky)for(unsigned kx=0;kx<8;++kx){float value=0;
                for(unsigned y=0;y<8;++y)value+=temporary[y][kx]*basis[ky][y];
                result[(ky*8+kx)*area+size_t(by)*cols+bx]=value;}
        }else{
            for(unsigned y=0;y<8;++y)for(unsigned kx=0;kx<8;++kx)for(unsigned ky=0;ky<8;++ky)
                temporary[y][kx]+=plane[(ky*8+kx)*area+size_t(by)*cols+bx]*basis[ky][y];
            for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){float value=0;
                for(unsigned k=0;k<8;++k)value+=temporary[y][k]*basis[k][x];
                result[size_t(by*8+y)*width+bx*8+x]=value;}
        }
    }});
    plane=std::move(result);
}
}
