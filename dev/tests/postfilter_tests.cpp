#include "fajk/codec.hpp"
#include "fajk/compute.hpp"
#include "../src/final_sharpen.hpp"
#include "../src/gpu_compute.hpp"
#include "../src/decode_internal.hpp"
#include "../src/reconstruction441.hpp"
#include <iostream>
#include <random>
#include <source_location>
using namespace fajk;
namespace {
void check(bool value,std::source_location at=std::source_location::current()){if(!value)throw std::runtime_error("Postfilter assertion at "+std::to_string(at.line()));}
Frame fixture(uint32_t w,uint32_t h){Frame frame{w,h,0,1,Bytes(pixelBytes(w,h),255)};
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto p=(size_t(y)*w+x)*4;
        frame.pixels[p]=uint8_t(40+(x/3+y/5)%160);frame.pixels[p+1]=uint8_t(30+(x/7+y/4)%170);frame.pixels[p+2]=uint8_t(20+(x/5+y/3)%180);}
    return frame;}
}
int main(){try{
    for(auto [w,h]:{std::pair{1u,1u},{1u,19u},{19u,1u},{7u,9u},{17u,23u},{63u,65u},{513u,517u}}){
        const auto frame=fixture(w,h);
        for(uint32_t strength:{0u,25u,50u,100u,150u,200u}){
            auto single=frame.pixels,multi=frame.pixels;
            finalSharpen(single,w,h,1,strength);finalSharpen(multi,w,h,3,strength);check(single==multi);
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){uint32_t actual;
                std::memcpy(&actual,single.data()+(size_t(y)*w+x)*4,4);check(actual==finalSharpPixel(frame.pixels,w,h,x,y,strength));}
            if(!strength)check(single==frame.pixels);
            for(size_t at=0;at<single.size();at+=4){
                check(single[at+3]==frame.pixels[at+3]);
                const int delta=int(single[at])-int(frame.pixels[at]);check(std::abs(delta)<=int((24*((strength*256+50)/100)+128)>>8));
                check(int(single[at+1])-int(frame.pixels[at+1])==delta&&int(single[at+2])-int(frame.pixels[at+2])==delta);
            }
            if(w>=512&&gpuInfo().available){auto gpu=frame.pixels;ScopedComputeBackend selected(ComputeBackend::Gpu);const auto activity=gpuActivity();
                finalSharpen(gpu,w,h,1,strength);check(gpu==single&&gpuActivity().fallbacks==activity.fallbacks);
                if(strength)check(gpuActivity().sharpens==activity.sharpens+1);}
        }
        auto transparent=frame;for(size_t at=3;at<transparent.pixels.size();at+=4)transparent.pixels[at]=uint8_t(at%255);
        auto p=transparent.pixels;finalSharpen(p,w,h,3,100);check(p==transparent.pixels);
        Bytes flat(frame.pixels.size());for(size_t at=0;at<flat.size();at+=4){flat[at]=70;flat[at+1]=90;flat[at+2]=120;flat[at+3]=255;}
        const auto original=flat;finalSharpen(flat,w,h,3,100);check(flat==original);
    }
    Frame peak{3,3,0,1,Bytes(pixelBytes(3,3),90)};for(size_t at=3;at<peak.pixels.size();at+=4)peak.pixels[at]=255;
    for(int c=0;c<3;++c)peak.pixels[(1*3+1)*4+c]=100;
    auto stronger=peak.pixels;finalSharpen(stronger,3,3,1,100);check(stronger[(1*3+1)*4]==101);
    auto doubled=peak.pixels;finalSharpen(doubled,3,3,1,200);check(doubled[(1*3+1)*4]==102);
    // Local transparency at vector and row-band boundaries must protect the
    // same five-pixel neighbourhood, with original luma on both sides.
    for(auto [w,h]:{std::pair{17u,67u},std::pair{641u,449u}}){
        auto mixed=fixture(w,h);std::mt19937 random(610);
        for(size_t at=0;at<mixed.pixels.size();at+=4){for(unsigned c=0;c<3;++c)mixed.pixels[at+c]=uint8_t(random());
            if((at/4)%37==0)mixed.pixels[at+3]=127;}
        for(uint32_t strength:{50u,100u,200u}){
            auto actual=mixed.pixels;finalSharpen(actual,w,h,10,strength);
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){uint32_t value;std::memcpy(&value,actual.data()+(size_t(y)*w+x)*4,4);
                check(value==finalSharpPixel(mixed.pixels,w,h,x,y,strength));}
            if(uint64_t(w)*h>=512*512&&gpuInfo().available){auto gpu=mixed.pixels;ScopedComputeBackend backend(ComputeBackend::Gpu);
                check(gpuFinalSharpen(gpu,w,h,strength)&&gpu==actual);}
        }
    }
    // Verify filter order independently: the public decoder must equal the
    // fully reconstructed image followed by one final pass, with every budget.
    auto frame=fixture(513,517);auto codec=makeCodec(CodecId::Fajk);codec->setThreads(3);
    EncodeOptions options;options.speed=EncodeSpeed::Fast;options.threads=3;
    for(uint32_t budget:{30000u,80000u,150000u}){
        options.targetBytes=budget;const auto packet=codec->encode(frame,options);
        auto base=decodeFrameWithSettings(packet,frame.width,frame.height,3,100,0);
        auto expected=base.pixels;finalSharpen(expected,frame.width,frame.height,3,100);
        auto actual=decodeFrameWithSettings(packet,frame.width,frame.height,3,100,100);check(actual.pixels==expected&&expected!=base.pixels);
        Bytes guarded(base.pixels.size()+32,197);decodeFrameToBuffer(packet,frame.width,frame.height,3,100,std::span(guarded).subspan(16,base.pixels.size()),100);
        check(std::equal(actual.pixels.begin(),actual.pixels.end(),guarded.begin()+16));for(int n=0;n<16;++n)check(guarded[n]==197&&guarded[guarded.size()-1-n]==197);
        auto off=packet;auto settings=read441(off.data,frame.width,frame.height).settings;settings.intensity=0;update441(off,settings);
        const auto noContours=codec->decode(off,frame.width,frame.height);check(noContours.pixels!=base.pixels);
    }
    std::cout<<"Postfilters: final order at 30/80/150 kB, live high-rate contours, SIMD/thread/GPU equality, alpha/flat/gamut/guard invariants PASS\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
