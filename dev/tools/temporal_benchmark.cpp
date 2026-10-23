// Paired intra/temporal tests, including container IO, reference decoding and
// copies. Source loading/generation, verification and PNG export are untimed.
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/media.hpp"
#include "fajk/compute.hpp"
#include "../src/temporal.hpp"
#include "../src/decode_internal.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <windows.h>
using namespace fajk;
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
uint64_t cpu(){FILETIME creation,exit,kernel,user;GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user);ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return k.QuadPart+u.QuadPart;}
struct Scenario{std::string name;std::vector<std::shared_ptr<const Frame>> frames;};
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==4,"temporal_benchmark PHOTO.png VIDEO.mp4 OUTPUT_DIRECTORY");MediaRuntime runtime;
    const Path output=argv[3];std::filesystem::create_directories(output);
    auto image=readImage(argv[1]);
    Scenario still{"static",{}},local{"local",{}},video{"motion",{}};
    for(uint32_t n=0;n<24;++n){
        auto frame=image;frame.pts=int64_t(n)*333333;frame.duration=333333;still.frames.push_back(std::make_shared<Frame>(frame));
        if(n){const uint32_t left=100+(n%6)*19,top=100+(n%3)*11;
            for(uint32_t y=top;y<std::min(top+48,frame.height);++y)for(uint32_t x=left;x<std::min(left+64,frame.width);++x){const auto at=(size_t(y)*frame.width+x)*4;frame.pixels[at]=235;frame.pixels[at+1]=40;frame.pixels[at+2]=80;}}
        local.frames.push_back(std::make_shared<Frame>(std::move(frame)));
    }
    MediaSource source(argv[2]);Frame f;int64_t origin=0;for(int n=0;n<24&&source.next(f);++n){if(!n)origin=f.pts;f.pts-=origin;video.frames.push_back(std::make_shared<Frame>(std::move(f)));}
    require(video.frames.size()==24,"Need 24 source frames.");
    std::ofstream csv(output/L"timings.csv");csv<<"scenario,round,backend,temporal,frames,keys,bytes,encode_ms,encode_cpu_ms,decode_ms,decode_cpu_ms,gpu_fallbacks,max_anchor_error_increase\n";
    for(const auto* scenario:{&still,&local,&video})for(int round=0;round<2;++round)for(int variant=0;variant<4;++variant){
        const int mode=(variant+round*2)%4;const auto backend=mode/2?ComputeBackend::Gpu:ComputeBackend::Cpu;const bool temporal=mode%2;
        EncodeOptions options;options.speed=EncodeSpeed::Fast;options.backend=backend;options.threads=10;options.targetBytes=80000;
        auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);codec->setComputeBackend(backend);
        const auto& frames=scenario->frames;const auto& first=*frames.front();for(int n=0;n<2;++n)codec->encode(first,options);
        const auto name=scenario->name+"-"+(backend==ComputeBackend::Gpu?"gpu":"cpu")+(temporal?"-temporal":"-intra");
        const auto file=output/(name+".fajkv");
        TemporalEncoder encoder(options,8);SequenceInfo info{first.width,first.height,30,1,CodecId::Fajk,1,0,0,temporal};
        const auto activity=gpuActivity();const auto encodeCpu=cpu();auto start=Clock::now();
        uint64_t keys=0;
        {SequenceWriter writer(file,info,true);
            for(size_t index=0;index<frames.size();++index){auto delta=temporal?encoder.encode(frames[index],index):std::optional<Packet>{};
                Packet packet;if(delta)packet=std::move(*delta);else{packet=codec->encode(*frames[index],options);++keys;}writer.append(packet);}
            writer.finish();}
        const auto encodeMs=ms(start),encodeCpuMs=(cpu()-encodeCpu)/10000.;
        SequenceReader reader(file);Frame decoded{first.width,first.height,0,1,Bytes(pixelBytes(first.width,first.height))};
        auto decode=[&](const Packet& p,ComputeBackend selected){decodeFrameToBuffer(p,first.width,first.height,10,100,decoded.pixels,0,true,selected);};
        decode(reader.read(0),backend);const auto decodeCpu=cpu();start=Clock::now();
        for(size_t index=0;index<frames.size();++index)decode(reader.read(index),backend);
        const auto decodeMs=ms(start),decodeCpuMs=(cpu()-decodeCpu)/10000.;
        require(gpuActivity().fallbacks==activity.fallbacks,"GPU error fallback.");
        unsigned errorIncrease=0;std::vector<uint32_t> checksums;
        for(size_t index=0;index<frames.size();++index){
            const auto packet=reader.read(index);decode(packet,backend);checksums.push_back(crc32(decoded.pixels));
            require(packet.pts==frames[index]->pts&&packet.duration==frames[index]->duration,"Timestamp changed.");
            require(packet.data.size()<=80000,"Packet exceeds cap.");
            if(packet.reference){
                const auto anchorIndex=packet.referenceIndex;
                const auto key=codec->decode(*packet.reference,first.width,first.height);
                for(size_t at=0;at<decoded.pixels.size();++at){
                    const int before=std::abs(int(key.pixels[at])-int(frames[size_t(anchorIndex)]->pixels[at]));
                    const int after=std::abs(int(decoded.pixels[at])-int(frames[index]->pixels[at]));
                    errorIncrease=std::max(errorIncrease,unsigned(std::max(0,after-before)));
                }
                require(errorIncrease==0,"Delta increased anchor error.");
            }
            if(index==0||index==12||index==frames.size()-1){writePng(output/(name+"-"+std::to_string(index)+".png"),decoded);writePng(output/(scenario->name+"-source-"+std::to_string(index)+".png"),*frames[index]);}
            // Every temporal frame is checked on the other backend as well.
            if(temporal){const auto expected=decoded.pixels;decode(packet,backend==ComputeBackend::Gpu?ComputeBackend::Cpu:ComputeBackend::Gpu);
                uint64_t difference=0;for(size_t at=0;at<expected.size();++at)difference+=unsigned(std::abs(int(decoded.pixels[at])-int(expected[at])));
                require(double(difference)/expected.size()<.001,"CPU/GPU temporal decode regression.");}
        }
        for(size_t index:{frames.size()-1,size_t(0),frames.size()/2}){decode(reader.read(index),backend);require(crc32(decoded.pixels)==checksums[index],"Seek/cache changed pixels.");}
        if(temporal&&scenario==&still)require(keys==1,"Static video forced keyframes.");
        csv<<scenario->name<<','<<round<<','<<(backend==ComputeBackend::Gpu?"gpu":"cpu")<<','<<temporal<<','<<frames.size()<<','<<keys<<','<<std::filesystem::file_size(file)<<','<<encodeMs<<','<<encodeCpuMs<<','<<decodeMs<<','<<decodeCpuMs<<','<<gpuActivity().fallbacks-activity.fallbacks<<','<<errorIncrease<<'\n';csv.flush();
        std::cout<<name<<" round "<<round<<": "<<encodeMs<<" ms encode, "<<decodeMs<<" ms decode, "<<keys<<" keys\n";
    }
    std::cout<<"PASS: every packet, anchor error bound, CPU/GPU cross decode, arbitrary seeking, static keyframe extension\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
