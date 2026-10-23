// Reproducible test of whether approximate reuse actually avoids full encoding.
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/media.hpp"
#include "fajk/compute.hpp"
#include "fajk/entropy.hpp"
#include "../src/temporal.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace fajk;
using Clock=std::chrono::steady_clock;
int wmain(int argc,wchar_t** argv){try{
    require(argc==4||argc==5,"similar_benchmark PHOTO VIDEO OUTPUT [FOCUSED_TOLERANCE]");MediaRuntime runtime;const Path output=argv[3];std::filesystem::create_directories(output);
    std::vector<Frame> motion,lighting,local;MediaSource media(argv[2]);Frame frame;
    for(int i=0;i<24&&media.next(frame);++i)motion.push_back(std::move(frame));require(motion.size()==24,"Need 24 frames.");
    const auto photo=readImage(argv[1]);for(uint32_t i=0;i<24;++i){auto f=photo;f.pts=i*333333;f.duration=333333;
        const int brightness=int(i%7)/2;for(size_t p=0;p<f.pixels.size();p+=4)for(size_t c=0;c<3;++c)f.pixels[p+c]=uint8_t(std::min(255,int(f.pixels[p+c])+brightness));
        auto l=f;if(i){const uint32_t left=300+(i%6)*19,top=300+(i%3)*11;for(uint32_t y=top;y<std::min(top+48,l.height);++y)for(uint32_t x=left;x<std::min(left+64,l.width);++x){const auto at=(size_t(y)*l.width+x)*4;l.pixels[at]=235;l.pixels[at+1]=40;l.pixels[at+2]=80;}}local.push_back(std::move(l));lighting.push_back(std::move(f));}
    std::ofstream csv(output/L"timings.csv");csv<<"scenario,round,tolerance,frames,keys,corrections,repeats,bytes,encode_ms,decode_ms,temporal_ms,gpu_fallbacks\n";
    for(int scenario=argc==5?2:0;scenario<3;++scenario)for(int round=0;round<2;++round)for(int variant=0;variant<(argc==5?1:5);++variant){
        const uint32_t modes[]={UINT32_MAX,0,3,6,12};const auto tolerance=argc==5?uint32_t(std::stoul(argv[4])):modes[(variant+round*2)%5];
        const auto& original=scenario==2?motion:scenario==1?local:lighting;const std::string scenarioName=scenario==2?"motion":scenario==1?"local":"lighting";std::vector<std::shared_ptr<Frame>> frames;for(const auto& f:original)frames.push_back(std::make_shared<Frame>(f));
        EncodeOptions options;options.backend=ComputeBackend::Gpu;options.speed=EncodeSpeed::Fast;options.targetBytes=80000;options.threads=10;
        auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);codec->setComputeBackend(ComputeBackend::Gpu);for(int i=0;i<2;++i)codec->encode(*frames[0],options);
        const bool temporal=tolerance!=UINT32_MAX;TemporalEncoder encoder(options,120,temporal?tolerance:0,10);
        uint64_t keys=0,corrections=0,repeats=0;double compareMs=0;
        const auto name=scenarioName+"-t"+(temporal?std::to_string(tolerance):"intra");
        const auto activity=gpuActivity();auto start=Clock::now();
        {SequenceInfo info{photo.width,photo.height,30,1,CodecId::Fajk,1,0,0,temporal};info.width=frames[0]->width;info.height=frames[0]->height;
        SequenceWriter writer(output/(name+".fajkv"),info,true);
        for(size_t i=0;i<frames.size();++i){auto t=Clock::now();auto delta=temporal?encoder.encode(frames[i],i):std::optional<Packet>{};compareMs+=std::chrono::duration<double,std::milli>(Clock::now()-t).count();Packet packet;
            if(delta){corrections+=delta->data[4]==temporalCorrectionVersion;repeats+=delta->data[4]==temporalPacketVersion&&delta->data.size()==20;}
            if(delta)packet=std::move(*delta);else{packet=codec->encode(*frames[i],options);if(temporal)encoder.commitKey(packet,i);++keys;}writer.append(packet);}
        writer.finish();}
        const double encodeMs=std::chrono::duration<double,std::milli>(Clock::now()-start).count();require(gpuActivity().fallbacks==activity.fallbacks,"GPU fallback.");double decodeMs;
        {SequenceReader reader(output/(name+".fajkv"));start=Clock::now();for(size_t i=0;i<frames.size();++i)codec->decode(reader.read(i),frames[i]->width,frames[i]->height);decodeMs=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        for(auto index:{0u,12u,23u}){const auto p=reader.read(index);const auto decoded=codec->decode(p,frames[index]->width,frames[index]->height);
            if(p.reference&&tolerance){const auto key=codec->decode(*p.reference,decoded.width,decoded.height);const auto& keySource=original[size_t(p.referenceIndex)];const uint32_t cols=(decoded.width+15)/16;std::vector<uint8_t> updated(size_t(cols)*((decoded.height+15)/16));
                if(p.data[4]==temporalCorrectionVersion){ByteReader in{p.data};in.bytes(16);const auto count=in.u32();for(uint32_t n=0;n<count;++n){const auto tile=in.u32(),size=in.u32();updated.at(tile)=1;in.bytes(size);}in.end();}
                for(uint32_t y=0;y<decoded.height;++y)for(uint32_t x=0;x<decoded.width;++x)for(size_t c=0;c<4;++c){const size_t at=(size_t(y)*decoded.width+x)*4+c;
                    if(updated[size_t(y/16)*cols+x/16])require(decoded.pixels[at]==original[index].pixels[at],"Correction did not restore original source pixel.");
                    else{require(decoded.pixels[at]==key.pixels[at],"Retained pixel changed.");require(std::abs(int(original[index].pixels[at])-int(keySource.pixels[at]))<=int(c==3?0:tolerance),"Retained pixel exceeded tolerance.");}}
            }
            writePng(output/(name+"-"+std::to_string(index)+".png"),decoded);writePng(output/(scenarioName+"-source-"+std::to_string(index)+".png"),original[index]);}}
        csv<<scenarioName<<','<<round<<','<<(temporal?std::to_string(tolerance):"intra")<<",24,"<<keys<<','<<corrections<<','<<repeats<<','<<std::filesystem::file_size(output/(name+".fajkv"))<<','<<encodeMs<<','<<decodeMs<<','<<compareMs<<','<<gpuActivity().fallbacks-activity.fallbacks<<'\n';csv.flush();
        std::cout<<name<<" round "<<round<<": "<<encodeMs<<" ms, "<<keys<<" keys, "<<corrections<<" exact corrections, "<<repeats<<" repeats"<<std::endl;
    }return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
