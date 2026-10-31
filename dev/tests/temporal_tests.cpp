#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/entropy.hpp"
#include "fajk/pipeline.hpp"
#include "fajk/media.hpp"
#include "../src/temporal.hpp"
#include "../src/temporal_compare.hpp"
#include "../src/decode_internal.hpp"
#include <iostream>
#include <random>
#include <source_location>
#include <fstream>
#include <windows.h>

using namespace fajk;
namespace {
void check(bool value,std::source_location at=std::source_location::current()){
    if(!value)throw std::runtime_error("Temporal assertion at "+std::to_string(at.line()));
}
template<class F>void rejected(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
Frame fixture(){
    Frame f{259,197,0,333333,Bytes(pixelBytes(259,197))};
    std::mt19937 random(491);
    for(auto& value:f.pixels)value=uint8_t(random());
    return f;
}
void edit(Frame& f,int difference=7){
    for(uint32_t y=190;y<f.height;++y)for(uint32_t x=252;x<f.width;++x)
        for(uint32_t c=0;c<4;++c){const auto at=(size_t(y)*f.width+x)*4+c;f.pixels[at]=uint8_t(std::clamp(int(f.pixels[at])+difference,0,255));}
}
}
int wmain(int argc,wchar_t** argv){try{
    {
        std::mt19937 random(5001);
        for(uint32_t width:{1,2,3,4,5,7,15,16,17})for(uint32_t height:{1,3,16})
        for(uint32_t padding:{0,12})for(uint32_t offset:{0,1,7})for(uint32_t tolerance:{0,1,3,16})for(uint32_t mode=0;mode<6;++mode){
            const auto pitch=size_t(width)*4+padding;Bytes key(pitch*height+offset),current(key.size());
            for(auto& value:key)value=uint8_t(random());current=key;
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)for(uint32_t c=0;c<3;++c){const auto at=offset+y*pitch+x*4+c;
                if(mode)current[at]=uint8_t(std::clamp(int(key[at])+int(random()%(2*tolerance+1))-int(tolerance),0,255));}
            if(mode>=2){const auto at=offset+(random()%height)*pitch+(random()%width)*4+(mode==2?3:random()%3);
                const auto delta=mode==2?1:tolerance+1;current[at]=uint8_t(int(key[at])+(key[at]<128?int(delta):-int(delta)));}
            // Padding is deliberately unrelated and must not affect a match.
            for(uint32_t y=0;y<height;++y)for(uint32_t p=width*4;p<pitch;++p)current[offset+y*pitch+p]^=255;
            bool similar=true,different=false;
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)for(uint32_t c=0;c<4;++c){const auto at=offset+y*pitch+x*4+c;
                const auto delta=uint32_t(std::abs(int(current[at])-int(key[at])));similar&=delta<=(c==3?0:tolerance);different|=delta!=0;}
            const auto actual=compareTemporalBlock(current.data()+offset,key.data()+offset,pitch,width,height,tolerance);
            check(actual.similar==similar);if(similar)check(actual.different==different);
        }
        std::cout<<"Temporal block comparison: scalar oracle, RGB limits, alpha, odd borders, unaligned rows and padding PASS\n";
    }
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(2);
    {
        Frame source{259,197,0,333333,Bytes(pixelBytes(259,197),100)};for(size_t p=3;p<source.pixels.size();p+=4)source.pixels[p]=255;
        EncodeOptions option;option.speed=EncodeSpeed::Fast;option.targetBytes=8000;option.threads=2;
        TemporalEncoder approximate(option,120,3,10);check(!approximate.encode(source,0));
        auto keyPacket=std::make_shared<Packet>(codec->encode(source,option));const auto keyImage=codec->decode(*keyPacket,259,197);
        approximate.commitKey(*keyPacket,0);
        auto current=source;for(size_t p=0;p<current.pixels.size();p+=4)for(size_t c=0;c<3;++c)current.pixels[p+c]+=2;
        for(uint32_t y=192;y<197;++y)for(uint32_t x=256;x<259;++x)current.pixels[(size_t(y)*259+x)*4]=150;
        auto patch=approximate.encode(current,1);check(patch&&patch->data[4]==temporalCorrectionVersion);patch->reference=keyPacket;
        const auto output=codec->decode(*patch,259,197);
        for(uint32_t y=0;y<197;++y)for(uint32_t x=0;x<259;++x)for(size_t c=0;c<4;++c){const auto at=(size_t(y)*259+x)*4+c;
            const auto expected=x>=256&&y>=192?current.pixels[at]:keyImage.pixels[at];check(output.pixels[at]==expected);}

        Bytes guarded(output.pixels.size()+32,197);decodeFrameToBuffer(*patch,259,197,2,100,std::span(guarded).subspan(16,output.pixels.size()));
        check(std::equal(output.pixels.begin(),output.pixels.end(),guarded.begin()+16));for(int i=0;i<16;++i)check(guarded[i]==197&&guarded[guarded.size()-1-i]==197);
        check(bool(approximate.encode(current,10)));check(!approximate.encode(current,11));
        auto bad=*patch;bad.reference.reset();rejected([&]{codec->decode(bad,259,197);});
        for(size_t length=0;length<20;++length){bad=*patch;bad.data.resize(length);rejected([&]{codec->decode(bad,259,197);});}
        bad=*patch;bad.data.pop_back();rejected([&]{codec->decode(bad,259,197);});
        bad=*patch;bad.data[20]=255;rejected([&]{codec->decode(bad,259,197);});
        bad=*patch;bad.data[6]=0;rejected([&]{codec->decode(bad,259,197);});
        for(uint8_t mode:{1,2,3}){bad=*patch;bad.data[5]=mode;rejected([&]{codec->decode(bad,259,197);});}
        auto alphaOnly=source;alphaOnly.pixels.back()=254;
        TemporalEncoder alphaEncoder(option,120,16,10);check(!alphaEncoder.encode(source,0));alphaEncoder.commitKey(*keyPacket,0);
        auto alphaPatch=alphaEncoder.encode(alphaOnly,1);check(alphaPatch&&alphaPatch->data[4]==temporalCorrectionVersion);alphaPatch->reference=keyPacket;
        check(codec->decode(*alphaPatch,259,197).pixels.back()==254);
        // Fast may prefer a clean source key over a large correction set.
        // More quality can still exploit an unusually compressible set.
        auto large=source;for(uint32_t y=0;y<128;++y)for(uint32_t x=0;x<128;++x)large.pixels[(size_t(y)*259+x)*4]=150;
        const auto originalLarge=large.pixels;
        TemporalEncoder fastLarge(option,120,3,10);check(!fastLarge.encode(source,0));fastLarge.commitKey(*keyPacket,0);check(!fastLarge.encode(large,1));check(large.pixels==originalLarge);
        auto qualityOption=option;qualityOption.speed=EncodeSpeed::MoreQuality;
        TemporalEncoder qualityLarge(qualityOption,120,3,10);check(!qualityLarge.encode(source,0));qualityLarge.commitKey(*keyPacket,0);
        auto largePatch=qualityLarge.encode(large,1);check(bool(largePatch));largePatch->reference=keyPacket;
        const auto largeDecoded=codec->decode(*largePatch,259,197);
        for(uint32_t y=0;y<128;++y)for(uint32_t x=0;x<128;++x)for(size_t c=0;c<4;++c){const size_t at=(size_t(y)*259+x)*4+c;check(largeDecoded.pixels[at]==large.pixels[at]);}
        const auto path=std::filesystem::temp_directory_path()/L"fajk-correction-test.fajkv";
        {SequenceInfo info{259,197,30,1,CodecId::Fajk,1,0,0,true};SequenceWriter writer(path,info,true);writer.append(*keyPacket);patch->pts=333333;writer.append(*patch);writer.finish();}
        {SequenceReader reader(path);auto key=reader.read(0);check(key.keyIdentity==reader.read(1).reference);
        check(codec->decode(reader.read(1),259,197).pixels==output.pixels);check(codec->decode(reader.read(0),259,197).pixels==keyImage.pixels);check(codec->decode(reader.read(1),259,197).pixels==output.pixels);
        key.data=codec->encode(current,option).data;codec->decode(key,259,197);check(codec->decode(reader.read(1),259,197).pixels==output.pixels);}
        std::filesystem::remove(path);
    }
    const auto base=fixture();
    for(bool lossless:{false,true}){
        EncodeOptions options;options.speed=EncodeSpeed::Fast;options.threads=2;options.lossless=lossless;options.targetBytes=lossless?0:100000;
        // Alpha is deliberately nonopaque; this also exercises exact alpha deltas.
        TemporalEncoder encoder(options,3);
        check(!encoder.encode(base,0));
        auto reference=std::make_shared<Packet>(codec->encode(base,options));
        const auto reconstructed=codec->decode(*reference,base.width,base.height);
        std::vector<Packet> packets{*reference};
        auto repeat=encoder.encode(base,1);check(repeat&&repeat->data.size()==20&&repeat->referenceIndex==0);packets.push_back(*repeat);
        auto changed=base;edit(changed);
        auto delta=encoder.encode(changed,2);check(delta&&delta->data.size()<8000&&delta->referenceIndex==0);packets.push_back(*delta);
        delta->reference=reference;
        const auto output=codec->decode(*delta,base.width,base.height);
        for(size_t i=0;i<base.pixels.size();++i){
            const auto expected=std::clamp(int(reconstructed.pixels[i])+int(changed.pixels[i])-int(base.pixels[i]),0,255);
            check(output.pixels[i]==expected);
            check(std::abs(int(output.pixels[i])-int(changed.pixels[i]))<=std::abs(int(reconstructed.pixels[i])-int(base.pixels[i])));
        }
        if(lossless)check(output.pixels==changed.pixels);
        // Identical pictures defer refresh beyond the interval, without decoding a chain.
        auto held=encoder.encode(changed,300);check(held&&held->data==delta->data&&held->referenceIndex==0);packets.push_back(*held);
        auto tinyChange=changed;edit(tinyChange,1);check(bool(encoder.encode(tinyChange,301)));
        auto next=changed;for(uint32_t y=0;y<32;++y)for(uint32_t x=0;x<32;++x)next.pixels[(size_t(y)*next.width+x)*4]^=63;
        check(!encoder.encode(next,302));
        auto staticAgain=encoder.encode(next,5000);check(staticAgain&&staticAgain->referenceIndex==302);
        auto full=next;for(auto& value:full.pixels)value^=127;check(!encoder.encode(full,5001));
        // An unlimited interval still refreshes on widespread change.
        TemporalEncoder unlimited(options,0);check(!unlimited.encode(base,0));check(bool(unlimited.encode(changed,100000)));
        check(!unlimited.encode(full,100001));
        const auto path=std::filesystem::temp_directory_path()/(lossless?L"fajk-temporal-lossless.fajkv":L"fajk-temporal.fajkv");
        SequenceInfo info{base.width,base.height,30,1,CodecId::Fajk,1,0,0,true};
        {SequenceWriter writer(path,info,true);for(size_t n=0;n<packets.size();++n){packets[n].pts=int64_t(n)*333333;packets[n].duration=333333;writer.append(packets[n]);}writer.finish();}
        {SequenceReader reader(path);check(reader.info().temporal&&reader.info().count==4);
        // Random access starts with only keyframe + selected packet, including padded borders.
        for(uint64_t index:{3,1,2,0,3}){
            auto packet=reader.read(index);auto decoded=codec->decode(packet,base.width,base.height);
            check(decoded.pixels==(index>=2?output.pixels:reconstructed.pixels));
            Bytes guarded(decoded.pixels.size()+32,197);
            decodeFrameToBuffer(packet,base.width,base.height,2,100,std::span(guarded).subspan(16,decoded.pixels.size()));
            check(std::equal(decoded.pixels.begin(),decoded.pixels.end(),guarded.begin()+16));
            for(int n=0;n<16;++n)check(guarded[n]==197&&guarded[guarded.size()-1-n]==197);
        }
        }
        auto bad=*delta;bad.reference.reset();rejected([&]{codec->decode(bad,base.width,base.height);});
        for(size_t length=0;length<delta->data.size();++length){bad=*delta;bad.data.resize(length);rejected([&]{codec->decode(bad,base.width,base.height);});}
        bad=*delta;bad.data.push_back(0);rejected([&]{codec->decode(bad,base.width,base.height);});
        bad=*delta;bad.data[20]=255;rejected([&]{codec->decode(bad,base.width,base.height);});
        bad=*delta;bad.reference=std::make_shared<Packet>(*delta);rejected([&]{codec->decode(bad,base.width,base.height);});
        ByteWriter invalid;invalid.u32(0x334b4a46);invalid.u8(132);invalid.u8(1);invalid.u16(0);invalid.u32(base.width);invalid.u32(base.height);invalid.u32(1);
        invalid.u32(0);const auto malformed=entropyEncode(Bytes(64*64*8,255));invalid.u32(uint32_t(malformed.size()));invalid.bytes(malformed);
        bad=*delta;bad.data=std::move(invalid.data);rejected([&]{codec->decode(bad,base.width,base.height);});
        // Writer rejects chains, self references and payloads whose dependency is hidden.
        {SequenceWriter writer(path.wstring()+L".invalid",info,true);writer.append(*reference);bad=*delta;bad.referenceIndex=1;rejected([&]{writer.append(bad);});bad.referenceIndex=UINT64_MAX;rejected([&]{writer.append(bad);});}
        std::filesystem::remove(path);
    }
    // Opaque lossy wavelet reference: clamp(add(diff)) cannot increase the
    // absolute keyframe reconstruction error, and repeated frames never drift.
    auto photo=base;for(size_t i=3;i<photo.pixels.size();i+=4)photo.pixels[i]=255;
    EncodeOptions options;options.speed=EncodeSpeed::Fast;options.targetBytes=8000;options.threads=2;
    TemporalEncoder encoder(options,120);check(!encoder.encode(photo,0));
    auto reference=std::make_shared<Packet>(codec->encode(photo,options));
    const auto key=codec->decode(*reference,photo.width,photo.height);
    auto changed=photo;edit(changed);auto delta=encoder.encode(changed,1);check(bool(delta));delta->reference=reference;
    auto decoded=codec->decode(*delta,photo.width,photo.height);
    for(size_t i=0;i<photo.pixels.size();++i)check(std::abs(int(decoded.pixels[i])-int(changed.pixels[i]))<=std::abs(int(key.pixels[i])-int(photo.pixels[i])));
    // Cache invalidation on settings, dimensions, backend/reference changes.
    codec->setDeblocking(0);const auto unfilteredKey=codec->decode(*reference,photo.width,photo.height);decoded=codec->decode(*delta,photo.width,photo.height);
    for(size_t i=0;i<photo.pixels.size();++i)check(decoded.pixels[i]==std::clamp(int(unfilteredKey.pixels[i])+int(changed.pixels[i])-int(photo.pixels[i]),0,255));
    // Tiny budgets must fall back to a key rather than overrun the packet cap.
    options.targetBytes=20;TemporalEncoder tiny(options,0);check(!tiny.encode(photo,0));check(!tiny.encode(changed,1));
    {
        // Exercise the actual conversion boundary: automatic still images
        // retain 4:4:4, explicit 4:2:0 wins, and GPU worker work is not lost
        // from progress merely because no compute happened on the caller.
        MediaRuntime runtime;
        const auto prefix=std::filesystem::temp_directory_path()/(L"fajk-still-pipeline-"+std::to_wstring(GetCurrentProcessId()));
        const Path png=prefix.wstring()+L".png",image=prefix.wstring()+L".fajk";
        writePng(png,photo);ConvertOptions config;config.replace=true;config.workers=2;config.encode=options;config.encode.targetBytes=12000;
        config.encode.chroma420=true;config.autoChroma=true;
        auto direct=config.encode;direct.threads=2;direct.chroma420=false;
        const auto expected444=codec->encode(photo,direct);const auto result=convert(png,image,config,{});
        check(result.frames==1&&result.gpu.transforms==0);
        {SequenceReader reader(image);check(reader.read(0).data==expected444.data);}
        config.autoChroma=false;direct.chroma420=true;const auto expected420=codec->encode(photo,direct);convert(png,image,config,{});
        {SequenceReader reader(image);check(reader.read(0).data==expected420.data);}
        if(gpuInfo().available){Frame large{641,449,0,333333,Bytes(pixelBytes(641,449),255)};
            for(uint32_t y=0;y<large.height;++y)for(uint32_t x=0;x<large.width;++x){const auto at=(size_t(y)*large.width+x)*4;
                large.pixels[at]=uint8_t(x+y);large.pixels[at+1]=uint8_t(x*3+y/2);large.pixels[at+2]=uint8_t(x/3+y*5);}
            writePng(png,large);config.encode.backend=ComputeBackend::Gpu;config.encode.targetBytes=400000;config.workers=4;
            const auto before=gpuActivity();const auto gpuResult=convert(png,image,config,{});const auto after=gpuActivity();
            check(after.uploadedBytes==before.uploadedBytes&&gpuResult.gpu.uploadedBytes>0);
        }
        std::filesystem::remove(png);std::filesystem::remove(image);
    }
    if(argc==2){
        MediaRuntime runtime;
        const auto path=std::filesystem::temp_directory_path()/L"fajk-temporal-pipeline.fajkv";
        {std::ofstream existing(path,std::ios::binary);existing<<"preserve destination";}
        ConvertOptions config;config.temporal=true;config.keyframeInterval=4;config.encode.speed=EncodeSpeed::Fast;
        config.encode.targetBytes=12000;config.workers=10;config.memoryMiB=256;config.replace=true;
        std::stop_source cancellation;
        bool progressed=false;
        rejected([&]{convert(argv[1],path,config,cancellation.get_token(),[&](const Progress&){progressed=true;cancellation.request_stop();});});
        check(progressed&&cancellation.stop_requested());
        {std::ifstream existing(path,std::ios::binary);std::string contents((std::istreambuf_iterator<char>(existing)),{});check(contents=="preserve destination");}
        config.every=4;convert(argv[1],path,config,{});
        std::vector<int64_t> expectedPts;int64_t sourceEnd=0;{MediaSource source(argv[1]);Frame frame;uint64_t n=0;
            while(source.next(frame)){if(n++%4==0)expectedPts.push_back(frame.pts);sourceEnd=frame.pts+frame.duration;}}
        {SequenceReader reader(path);check(reader.info().temporal&&reader.info().count==8&&reader.info().sourceStep==4);
            for(uint64_t n=0;n<8;++n){auto packet=reader.read(n);check(packet.pts==expectedPts[n]&&packet.duration==(n+1<8?expectedPts[n+1]:sourceEnd)-packet.pts);codec->decode(packet,reader.info().width,reader.info().height);}}
        std::filesystem::remove(path);
        config.maxFrameCount=1;config.every=1;const auto image=path.wstring()+L".fajk";convert(argv[1],image,config,{});
        {SequenceReader reader(image);check(!reader.info().temporal&&reader.info().count==1);}
        std::filesystem::remove(image);
        config.maxFrameCount=0;config.similarTolerance=3;config.similarHoldFrames=10;
        for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu}){
            config.encode.backend=backend;convert(argv[1],path,config,{});
            SequenceReader reader(path);check(reader.info().temporal&&reader.info().count==32);
            codec->setComputeBackend(backend);uint64_t corrections=0;
            for(uint64_t n=0;n<32;++n){auto packet=reader.read(n);corrections+=packet.data[4]==temporalCorrectionVersion;codec->decode(packet,reader.info().width,reader.info().height);}
            check(corrections>0);
        }
        config.similarTolerance=0;config.coefficientTemporal=true;config.keyframeInterval=4;
        convert(argv[1],path,config,{});
        {SequenceReader reader(path);check(reader.info().temporal&&reader.info().count==32);
            for(uint64_t n=0;n<32;++n){const auto packet=reader.read(n);codec->decode(packet,reader.info().width,reader.info().height);}}
        std::filesystem::remove(path);
        std::cout<<"Temporal pipeline: cancellation preserves destination, 256 MiB path, sampling, one-frame compatibility PASS\n";
        config.coefficientTemporal=false;config.maxCompressionTemporal=true;config.similarTolerance=6;config.maxFrameCount=12;config.memoryMiB=4096;config.workers=10;
        for(bool overlap:{false,true}){
            config.overlapMotionPreparation=overlap;
            const auto result=convert(argv[1],path,config,{});check(result.frames==12&&(!result.preparationWorkers||overlap));
            {SequenceReader reader(path);check(reader.info().temporalChains&&reader.info().count==12);
                for(uint64_t n=0;n<12;++n){const auto packet=reader.read(n);const auto decoded=codec->decode(packet,reader.info().width,reader.info().height);check(decoded.pts==packet.pts&&decoded.duration==packet.duration);}}
            std::filesystem::remove(path);
        }
        {std::ofstream existing(path,std::ios::binary);existing<<"preserve Max destination";}
        std::stop_source maxStop;rejected([&]{convert(argv[1],path,config,maxStop.get_token(),[&](const Progress&){maxStop.request_stop();});});
        {std::ifstream existing(path,std::ios::binary);std::string contents((std::istreambuf_iterator<char>(existing)),{});check(contents=="preserve Max destination");}
        std::filesystem::remove(path);
        std::cout<<"Max pipeline: serial/overlap, ordered frames, closed references, cancellation preserves destination PASS\n";
    }
    std::cout<<"Temporal: exact deltas, bounded error, repeat/age policy, fallback, random seek, guarded output, truncations PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
