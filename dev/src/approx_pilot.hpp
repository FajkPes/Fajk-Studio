#pragma once
#include "wavelet.hpp"
#include "fajk/hardware.hpp"
#include <array>
#include <bit>
#include <immintrin.h>

namespace fajk {
struct ApproximatePilotOptions {
    float baseStep=1,slope=166.f/256,diagonal=173.f/128,rounding=.5f,adaptiveRate=.02f;
    // 0: uniform band steps, 1: inexpensive coefficient-domain 16x16 AQ.
    uint8_t adaptive=0;
};
struct ApproximateLumaPilot {
    std::vector<float> coefficients;
    std::vector<float> spatial;
};
// A non-stream pilot: accepts the already transformed source and never calls
// range/entropy coding, rate-search packs or any packet decoder. The caller
// obtains baseStep from its existing image histogram/rate estimate.
inline ApproximateLumaPilot approximateLumaPilot(std::span<const float> transformed,uint32_t stride,uint32_t height,uint32_t threads,const ApproximatePilotOptions& options,bool reconstruction=true){
    require(stride>=64&&height>=64&&stride%64==0&&height%64==0&&transformed.size()==size_t(stride)*height,"Invalid approximate pilot dimensions.");
    require(std::isfinite(options.baseStep)&&options.baseStep>0&&options.baseStep<=4095&&std::isfinite(options.slope)&&options.slope>0&&options.slope<=1&&std::isfinite(options.diagonal)&&options.diagonal>0&&options.diagonal<=2&&std::isfinite(options.rounding)&&options.rounding>=0&&options.rounding<=1&&std::isfinite(options.adaptiveRate)&&options.adaptiveRate>=0&&options.adaptive<=1,"Invalid approximate pilot options.");
    ApproximateLumaPilot pilot;pilot.coefficients.resize(transformed.size());
    const auto sign=_mm_set1_ps(-0.f),bias=_mm_set1_ps(options.rounding);
    parallelFor(19,threads,[&](uint32_t job){
        if(job==18){const uint32_t width=stride>>6,rows=height>>6;const float step=std::max(.25f,options.baseStep*.1f);
            for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<width;++x){const size_t at=size_t(y)*stride+x;const float value=transformed[at];require(std::isfinite(value)&&std::abs(value)<=10000000,"Invalid approximate pilot sample.");pilot.coefficients[at]=std::round(value/step)*step;}return;}
        const uint32_t level=job/3,band=job%3,width=(stride>>level)/2,rows=(height>>level)/2,ox=band==1?0:width,oy=band==0?0:rows;
        const float bandStep=std::max(.35f,options.baseStep*(band==2?options.diagonal:1.f)*std::pow(options.slope,float(level)));
        for(uint32_t ty=0;ty<rows;ty+=16)for(uint32_t tx=0;tx<width;tx+=16){const uint32_t w=std::min(16u,width-tx),h=std::min(16u,rows-ty);float factor=1;
            if(options.adaptive){std::array<double,3> costs{.04,0,.04};
                for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const float value=transformed[size_t(oy+ty+y)*stride+ox+tx+x];require(std::isfinite(value)&&std::abs(value)<=10000000,"Invalid approximate pilot sample.");const float magnitude=std::abs(value/bandStep),importance=1+2*std::min(2.f,magnitude);
                    for(unsigned candidate=0;candidate<3;++candidate){const float scale=candidate==0?.5f:candidate==2?2.f:1.f;const unsigned q=unsigned(magnitude/scale+options.rounding);const double error=magnitude-q*scale;costs[candidate]+=importance*error*error+options.adaptiveRate*(q?3+2*std::bit_width(q):0);}}
                unsigned best=1;for(unsigned candidate:{0u,2u})if(costs[candidate]<costs[best])best=candidate;factor=best==0?.5f:best==2?2.f:1.f;
            }
            const float step=bandStep*factor,inverse=1/step;const auto inverse4=_mm_set1_ps(inverse),step4=_mm_set1_ps(step);
            for(uint32_t y=0;y<h;++y){const size_t at=size_t(oy+ty+y)*stride+ox+tx;uint32_t x=0;
                for(;x+4<=w;x+=4){const auto value=_mm_loadu_ps(transformed.data()+at+x);const auto magnitude=_mm_andnot_ps(sign,value);
                    // The accepted native coefficient range prevents a cast
                    // overflow and catches nonfinite source-transform output.
                    require(_mm_movemask_ps(_mm_cmple_ps(magnitude,_mm_set1_ps(10000000)))==15,"Invalid approximate pilot sample.");
                    const auto q=_mm_cvtepi32_ps(_mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(magnitude,inverse4),bias)));const auto signedQ=_mm_or_ps(q,_mm_and_ps(value,sign));_mm_storeu_ps(pilot.coefficients.data()+at+x,_mm_mul_ps(signedQ,step4));}
                for(;x<w;++x){const float value=transformed[at+x];require(std::isfinite(value)&&std::abs(value)<=10000000,"Invalid approximate pilot sample.");const float q=std::trunc(std::abs(value)*inverse+options.rounding);pilot.coefficients[at+x]=std::copysign(q*step,value);}
            }
        }
    });
    if(reconstruction){pilot.spatial=pilot.coefficients;waveletTransform(pilot.spatial,int(stride),int(height),threads,true);}return pilot;
}
}
