#pragma once
#include "resample.hpp"
namespace fajk {
void edgeUpscaleRow(const uint8_t*,uint32_t,uint32_t,uint8_t*,uint32_t,const int*,const float*,int,float,float,bool);
// Decoded pixels only. All channels share one geometric reconstruction; the
// result is bounded by the four central colors to prevent new color extrema.
inline Frame edgeUpscale(const Frame& source,uint32_t width,uint32_t height,uint32_t threads,std::span<uint8_t> destination={},float amount=1.f,bool boundary=false,bool cubic=false){
    source.validate();require(opaquePhoto(source.pixels)&&width>=source.width&&height>=source.height&&amount>=0&&amount<=1,"Invalid R342 upscale.");
    require(destination.empty()||destination.size()==pixelBytes(width,height),"Invalid R342 destination.");
    if(!hasAvx2())return resamplePhoto(source,width,height,threads,destination,2,boundary);
    Frame result{width,height,source.pts,source.duration,destination.empty()?Bytes(pixelBytes(width,height)):Bytes{}};auto pixels=destination.empty()?std::span(result.pixels):destination;
    struct Axis{uint32_t input=0,output=0;std::vector<int> low;std::vector<float> fraction;};thread_local Axis horizontal,vertical;
    auto prepare=[](Axis& a,uint32_t input,uint32_t output){if(a.input==input&&a.output==output)return;a.low.resize(output);a.fraction.resize(output);for(uint32_t i=0;i<output;++i){const double position=(i+.5)*input/output-.5;const int low=int(std::floor(position));a.low[i]=low;a.fraction[i]=float(position-low);}a.input=input;a.output=output;};
    prepare(horizontal,source.width,width);prepare(vertical,source.height,height);
    const auto& xLow=horizontal.low;const auto& xFraction=horizontal.fraction;const auto& yLow=vertical.low;const auto& yFraction=vertical.fraction;
    auto row=[&](uint32_t y,uint8_t* out){edgeUpscaleRow(source.pixels.data(),source.width,source.height,out,width,xLow.data(),xFraction.data(),yLow[y],yFraction[y],amount,cubic);};
    constexpr uint32_t stripeRows=32;parallelFor((height+stripeRows-1)/stripeRows,threads,[&](uint32_t job){const uint32_t begin=job*stripeRows,end=std::min(height,begin+stripeRows);
        if(!boundary){for(uint32_t y=begin;y<end;++y)row(y,pixels.data()+size_t(y)*width*4);return;}
        const auto first=begin>3?begin-3:0,last=std::min(height,end+3);thread_local Frame stripe;thread_local Bytes filtered;stripe.width=width;stripe.height=last-first;stripe.pixels.resize(pixelBytes(width,stripe.height));filtered.resize(stripe.pixels.size());
        for(uint32_t y=first;y<last;++y)row(y,stripe.pixels.data()+size_t(y-first)*width*4);
        restoreColorBoundaries(stripe,1,1.f,3.f,2,0,false,true,filtered,begin-first,end-first);std::copy_n(filtered.data()+size_t(begin-first)*width*4,size_t(end-begin)*width*4,pixels.data()+size_t(begin)*width*4);
    });return result;
}
}
