#include "fajk/media.hpp"
#include "fajk/codec.hpp"
#include "fajk/hardware.hpp"
#include "../src/frame_translation.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace fajk;
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
Frame canvas(const Frame& source,uint32_t x0,uint32_t y0){
    constexpr uint32_t padding=112;Frame out{source.width+padding,source.height+padding,source.pts,source.duration,Bytes(pixelBytes(source.width+padding,source.height+padding))};
    parallelFor(out.height,10,[&](uint32_t y){const auto sy=std::clamp(int(y)-int(y0),0,int(source.height)-1);const auto* input=source.pixels.data()+size_t(sy)*source.width*4;auto* target=out.pixels.data()+size_t(y)*out.width*4;
        std::memcpy(target+size_t(x0)*4,input,size_t(source.width)*4);
        for(uint32_t x=0;x<x0;++x)std::memcpy(target+size_t(x)*4,input,4);
        for(uint32_t x=x0+source.width;x<out.width;++x)std::memcpy(target+size_t(x)*4,input+size_t(source.width-1)*4,4);
    });return out;
}
Frame crop(const Frame& source,uint32_t x0,uint32_t y0,uint32_t width,uint32_t height){
    Frame out{width,height,source.pts,source.duration,Bytes(pixelBytes(width,height))};
    for(uint32_t y=0;y<height;++y)std::memcpy(out.pixels.data()+size_t(y)*width*4,source.pixels.data()+(size_t(y+y0)*source.width+x0)*4,size_t(width)*4);return out;
}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==3,"phase_sequence_trial VIDEO OUTPUT");MediaRuntime runtime;MediaSource media(argv[1]);const Path output=argv[2];std::filesystem::create_directories(output);
    EncodeOptions options;options.backend=ComputeBackend::Gpu;options.speed=EncodeSpeed::Fast;options.threads=10;options.targetBytes=80000;
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);codec->setComputeBackend(ComputeBackend::Gpu);
    std::ofstream csv(output/L"results.csv");csv<<"frame,dx,dy,phase_x,phase_y,bytes,intra_bytes,flicker,flicker_intra,stable_pixels,motion_ms,encode_ms,intra_encode_ms,decode_ms,intra_decode_ms\n";
    Frame source,previousSource,previousIntra,previousAligned;int phaseX=0,phaseY=0;uint64_t sum=0,intraSum=0;
    for(uint32_t n=0;n<48&&media.next(source);++n){
        auto start=Clock::now();const auto intra=codec->encode(source,options);const double intraEncode=elapsed(start);start=Clock::now();const auto ordinary=codec->decode(intra,source.width,source.height);const double intraDecode=elapsed(start);
        start=Clock::now();const auto motion=n?estimateFrameTranslation(source,previousSource):FrameTranslation{};
        if(motion.error>20){phaseX=0;phaseY=0;}else {phaseX=((phaseX+motion.x)%64+64)%64;phaseY=((phaseY+motion.y)%64+64)%64;}
        const auto x0=uint32_t(24+phaseX),y0=uint32_t(24+phaseY);const auto input=canvas(source,x0,y0);const double motionMs=elapsed(start);
        start=Clock::now();const auto packet=codec->encode(input,options);const double encode=elapsed(start);start=Clock::now();const auto decoded=codec->decode(packet,input.width,input.height);const auto aligned=crop(decoded,x0,y0,source.width,source.height);const double decode=elapsed(start);
        double flicker=0,intraFlicker=0;uint64_t stable=0;
        if(n)for(uint32_t y=128;y+128<source.height;++y)for(uint32_t x=128;x+128<source.width;++x){const int px=int(x)+motion.x,py=int(y)+motion.y;if(px<0||py<0||px>=int(source.width)||py>=int(source.height))continue;
            const auto at=(size_t(y)*source.width+x)*4,previous=(size_t(py)*source.width+px)*4;unsigned maximum=0;for(int c=0;c<3;++c)maximum=std::max(maximum,unsigned(std::abs(int(source.pixels[at+c])-int(previousSource.pixels[previous+c]))));
            if(maximum<=3)for(int c=0;c<3;++c){const int signal=int(source.pixels[at+c])-int(previousSource.pixels[previous+c]),e=int(aligned.pixels[at+c])-int(previousAligned.pixels[previous+c])-signal,b=int(ordinary.pixels[at+c])-int(previousIntra.pixels[previous+c])-signal;flicker+=e*e;intraFlicker+=b*b;++stable;}}
        sum+=packet.data.size();intraSum+=intra.data.size();csv<<n<<','<<motion.x<<','<<motion.y<<','<<phaseX<<','<<phaseY<<','<<packet.data.size()<<','<<intra.data.size()<<','<<(stable?std::sqrt(flicker/stable):0)<<','<<(stable?std::sqrt(intraFlicker/stable):0)<<','<<stable/3<<','<<motionMs<<','<<encode<<','<<intraEncode<<','<<decode<<','<<intraDecode<<'\n';csv.flush();
        if(n==0||n==1||n==5||n==12||n==23||n==47){const auto name=std::to_string(n);writePng(output/(name+"-source.png"),source);writePng(output/(name+"-intra.png"),ordinary);writePng(output/(name+"-aligned.png"),aligned);}
        previousSource=source;previousIntra=ordinary;previousAligned=aligned;
        std::cout<<n<<": grid "<<phaseX<<','<<phaseY<<", bytes "<<sum<<" / "<<intraSum<<std::endl;
    }return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
