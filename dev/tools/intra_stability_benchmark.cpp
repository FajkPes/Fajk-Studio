#include "fajk/media.hpp"
#include "fajk/codec.hpp"
#include "fajk/entropy.hpp"
#include "../src/wavelet_rate_control.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
using namespace fajk;
namespace {
struct Quantizer { float step=0;uint32_t width=0,height=0; };
Quantizer quantizer(std::span<const uint8_t> bytes){
    for(unsigned depth=0;depth<8;++depth){
        require(bytes.size()>=18,"Truncated diagnostic packet.");ByteReader in{bytes};in.bytes(4);const auto version=in.u8();
        if(version==114){in.bytes(3);const auto w=in.u32(),h=in.u32();return {in.u16()/16.f,w,h};}
        if(version==130){in.bytes(27);bytes=in.bytes(in.u32());}
        else if(version==129){in.bytes(19);bytes=in.bytes(in.u32());}
        else if(version==77){in.bytes(11);const auto map=in.u32(),size=in.u32();in.bytes(map);bytes=in.bytes(size);}
        else if(version==28){in.bytes(19);bytes=in.bytes(in.u32());}
        else if(version==55){in.bytes(2);in.bytes(in.var());bytes=in.bytes(in.data.size()-in.pos);}
        else return {};
    }throw std::runtime_error("Too many diagnostic wrappers.");
}
struct Difference {double rms=0;uint64_t changed=0,count=0;unsigned maximum=0;};
Difference difference(const Frame& a,const Frame& b,bool excludePatch=false){
    Difference d;
    for(uint32_t y=0;y<a.height;++y)for(uint32_t x=0;x<a.width;++x){
        if(excludePatch&&std::abs(int(x)-int(a.width/2))<160&&std::abs(int(y)-int(a.height/2))<160)continue;
        bool changed=false;for(unsigned c=0;c<3;++c){const auto at=(size_t(y)*a.width+x)*4+c;const unsigned error=unsigned(std::abs(int(a.pixels[at])-int(b.pixels[at])));d.rms+=error*error;d.maximum=std::max(d.maximum,error);changed|=bool(error);++d.count;}
        d.changed+=changed;
    }d.rms=d.count?std::sqrt(d.rms/d.count):0;return d;
}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc>=3&&argc<=5,"intra_stability_benchmark VIDEO OUTPUT [PREFERRED_STEP|-1=previous|-2=recovery] [FILTERS=0/1/2]");MediaRuntime runtime;MediaSource media(argv[1]);const Path out=argv[2];std::filesystem::create_directories(out);
    const float preference=argc>=4?std::stof(argv[3]):0;WaveletRateControl rate;rate.denseHistogram=preference==-3;rate.smoothColorModel=preference==-4;ScopedWaveletRateControl rateScope(preference?&rate:nullptr);
    Frame base;require(media.next(base),"Empty video.");EncodeOptions options;options.backend=ComputeBackend::Gpu;options.speed=EncodeSpeed::Fast;options.threads=10;options.targetBytes=80000;
    const int filters=argc>4?std::stoi(argv[4]):0;require(filters>=0&&filters<=2,"Invalid filter diagnostic.");
    if(filters){options.reconstruction441.intensity=0;options.reconstruction441.antialias=0;options.reconstruction441.contrast=0;}
    if(filters==2){options.shapeIntensity=0;options.contourIntensity=0;}
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);const auto packet=codec->encode(base,options);const auto reference=codec->decode(packet,base.width,base.height);
    std::ofstream csv(out/L"results.csv");csv<<"case,bytes,quantizer,working_width,working_height,packet_identical,changed_pixels,rms,max,remote_changed,remote_rms,remote_max,flicker,stable_pixels,encode_ms,decode_ms\n";
    Frame previousSource=base,previousResult=reference;size_t previousBytes=packet.data.size();
    auto record=[&](const std::string& name,const Frame& source,const EncodeOptions& config){rate.preferredStep=preference==-1||preference==-2?rate.actualStep*(preference==-2&&previousBytes<config.targetBytes*.97?.995f:1.f):std::max(0.f,preference);auto start=std::chrono::steady_clock::now();const auto p=codec->encode(source,config);const double encodeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();previousBytes=p.data.size();const auto q=quantizer(p.data);start=std::chrono::steady_clock::now();const auto decoded=codec->decode(p,base.width,base.height);const double decodeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();const auto all=difference(decoded,reference),remote=difference(decoded,reference,true);
        double flicker=0;uint64_t stable=0;
        if(name.starts_with("video"))for(size_t i=0;i<source.pixels.size();i+=4){unsigned maximum=0;for(int c=0;c<3;++c)maximum=std::max(maximum,unsigned(std::abs(int(source.pixels[i+c])-int(previousSource.pixels[i+c]))));if(maximum<=3)for(int c=0;c<3;++c){const int e=int(decoded.pixels[i+c])-int(previousResult.pixels[i+c])-int(source.pixels[i+c])+int(previousSource.pixels[i+c]);flicker+=e*e;++stable;}}
        csv<<name<<','<<p.data.size()<<','<<q.step<<','<<q.width<<','<<q.height<<','<<(p.data==packet.data)<<','<<all.changed<<','<<all.rms<<','<<all.maximum<<','<<remote.changed<<','<<remote.rms<<','<<remote.maximum<<','<<(stable?std::sqrt(flicker/stable):0)<<','<<stable/3<<','<<encodeMs<<','<<decodeMs<<'\n';csv.flush();std::cout<<name<<": step="<<q.step<<", rms="<<all.rms<<", remote="<<remote.rms<<std::endl;
        if(name.starts_with("video")){previousSource=source;previousResult=decoded;}
        if(name=="base"||name=="patch16"||name=="video1"||name=="video12"||name=="video23"||name=="video47"){writePng(out/(name+".png"),decoded);writePng(out/(name+"-source.png"),source);}};
    record("base",base,options);record("repeat",base,options);
    auto cpu=options;cpu.backend=ComputeBackend::Cpu;record("cpu",base,cpu);auto single=options;single.threads=1;record("single",base,single);
    for(uint32_t patch:{1,4,16,64}){auto changed=base;for(uint32_t y=0;y<patch;++y)for(uint32_t x=0;x<patch;++x){const auto at=(size_t(base.height/2+y)*base.width+base.width/2+x)*4;for(unsigned c=0;c<3;++c)changed.pixels[at+c]=uint8_t(std::min(255,int(changed.pixels[at+c])+2));}record("patch"+std::to_string(patch),changed,options);}
    Frame source;for(unsigned n=1;n<48&&media.next(source);++n)record("video"+std::to_string(n),source,options);
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
