#include "../src/playback_decoder.hpp"
#include "../src/sequence_decode_ahead.hpp"
#include "../src/decode_throughput.hpp"
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include <windows.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <cmath>

using namespace fajk;
void check(bool value,const char* message){require(value,message);}
DecodeResult waitFor(PlaybackDecoder& decoder,uint64_t generation){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(std::chrono::steady_clock::now()<deadline){if(auto result=decoder.takeDue(generation,false,0))return std::move(*result);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    throw std::runtime_error("Playback worker timed out.");
}
int main(){try{
    // Controlled timestamps distinguish overlap, producer idle time and task
    // latency without hardware-dependent sleep/assertions about performance.
    {
        using Clock=DecodeThroughput::Clock;
        const auto at=[](int64_t ms){return Clock::time_point{}+std::chrono::milliseconds(ms);};
        auto expectMs=[](double actual,double expected){check(std::abs(actual-expected)<1e-9,"Decode throughput measured the wrong interval.");};
        DecodeThroughput meter;
        check(meter.sample(at(0)).msPerFrame<0,"Empty throughput reported a speed.");
        meter.begin(at(0));meter.begin(at(0));meter.finish(at(26));meter.finish(at(26));
        expectMs(meter.sample(at(26)).msPerFrame,13);
        expectMs(meter.sample(at(1000)).msPerFrame,13); // Full queue / paused playback.
        meter.begin(at(1000));meter.begin(at(1010));meter.finish(at(1026));meter.finish(at(1036));
        expectMs(meter.sample(at(1036)).msPerFrame,15.5); // Union: (26 + 36) / 4.
        meter={};meter.begin(at(0));meter.begin(at(10));meter.finish(at(26));meter.finish(at(36));
        expectMs(meter.sample(at(36)).msPerFrame,18); // Not 26 / lane count.
        meter={};meter.begin(at(0));meter.finish(at(5),false);
        check(meter.sample(at(5)).frames==0&&meter.sample(at(5)).msPerFrame<0,"Failed decode counted as a finished frame.");
        meter={};
        for(int64_t n=0;n<64;++n){meter.begin(at(n*100));meter.finish(at(n*100+(n<32?26:10)));}
        const auto recent=meter.sample(at(10000));
        check(recent.frames==32,"Decode throughput window is not bounded.");expectMs(recent.msPerFrame,10);
        meter={};check(meter.sample(at(10000)).frames==0,"Throughput survived a seek reset.");
    }
    const auto path=std::filesystem::temp_directory_path()/(L"fajk-playback-"+std::to_wstring(GetCurrentProcessId())+L".fajkv");
    auto codec=makeCodec(CodecId::Fajk);EncodeOptions options;options.lossless=true;options.threads=2;
    std::vector<Frame> expected;
    {SequenceWriter writer(path,{129,97,30,1,CodecId::Fajk},true);
        for(uint32_t n=0;n<8;++n){Frame frame{129,97,n*333333ll,333333,Bytes(pixelBytes(129,97),255)};
            for(size_t at=0;at<frame.pixels.size();at+=4){frame.pixels[at]=uint8_t(at+n*17);frame.pixels[at+1]=uint8_t(at/7+n*31);frame.pixels[at+2]=uint8_t(at/13+n*43);}
            writer.append(codec->encode(frame,options));expected.push_back(std::move(frame));}writer.finish();}
    std::shared_ptr<Frame> retained;
    {
        std::atomic<uint64_t> epoch=1;
        SequenceDecodeAhead ahead(2);SequenceDecodeSettings settings;settings.path=path;settings.width=129;settings.height=97;settings.threads=2;settings.generation=1;
        for(uint32_t chunk:{1,2,4}){ahead.start(settings,0,3,1,5,chunk);
            for(uint64_t n=0;n<4;++n){auto result=ahead.take(n,1);check(result&&result->error.empty()&&result->image&&result->image->pixels==expected[n].pixels,"Decode-ahead reordered or changed pixels.");}
        }
        settings.borrowCpuWorkers=true;ahead.start(settings,0,3);
        for(uint64_t n=0;n<4;++n){auto result=ahead.take(n,1);check(result&&result->image&&result->image->pixels==expected[n].pixels,"Shared workers changed decoded pixels.");}
        ahead.start(settings,0,3,2);for(uint64_t n:{0,2}){auto result=ahead.take(n,1);check(result&&result->image->pixels==expected[n].pixels,"Decode-ahead sampling changed pixels.");}
        settings.includeLast=true;ahead.start(settings,0,3,2);
        for(uint64_t n:{0,2,3}){auto result=ahead.take(n,1);check(result&&result->frame==n&&result->image->pixels==expected[n].pixels,"Decode-ahead skipped the final sampled frame.");}
        settings.includeLast=false;
        settings.epoch=&epoch;
        for(uint64_t n=2;n<10;++n){epoch=n;settings.generation=n;ahead.start(settings,0,3);epoch=n+1;
            check(!ahead.take(0,n,&epoch),"Decode-ahead published a cancelled frame.");ahead.cancel();
            settings.generation=n+1;settings.contentRevision=n;ahead.start(settings,3,3);
            auto result=ahead.take(3,n+1,&epoch);check(result&&result->image->pixels==expected[3].pixels,"Decode-ahead retained stale results after reset.");retained=result->image;
        }
        settings.epoch=nullptr;settings.generation=20;settings.backend=ComputeBackend(99);ahead.start(settings,0,0);
        auto failed=ahead.take(0,20);check(failed&&!failed->error.empty()&&!failed->image&&failed->throughputFrames==0,"Decode-ahead failed to propagate worker errors.");
        settings.generation=21;settings.backend=ComputeBackend::Cpu;ahead.start(settings,0,0);
        auto recovered=ahead.take(0,21);check(recovered&&recovered->image->pixels==expected[0].pixels,"Decode-ahead did not recover after an error.");
        check(recovered->throughputFrames==1&&recovered->throughputMs>0,"Decode-ahead retained throughput from another generation.");
    }
    check(retained&&retained->pixels==expected[3].pixels,"Closing decode-ahead invalidated an owned frame.");
    {
        PlaybackDecoder decoder;DecodeRequest work;work.path=path;work.generation=1;work.threads=2;work.sharpening=0;work.last=3;work.continuous=true;
        decoder.invalidate(1);decoder.submit(work);
        for(uint32_t n=0;n<4;++n){const auto result=waitFor(decoder,1);check(result.error.empty()&&result.frame==n&&result.image&&result.image->pixels==expected[n].pixels,"Continuous playback changed frame order or pixels.");}
        work.continuous=false;work.generation=2;work.frame=3;decoder.invalidate(2);decoder.submit(work);
        work.generation=3;work.frame=1;decoder.invalidate(3);decoder.submit(work);
        const auto seek=waitFor(decoder,3);check(seek.error.empty()&&seek.frame==1&&seek.image->pixels==expected[1].pixels,"Invalidation published a stale frame.");
        work.generation=4;work.frame=999;decoder.invalidate(4);decoder.submit(work);check(!waitFor(decoder,4).error.empty(),"Out-of-range playback did not report an error.");
        work.generation=5;work.frame=0;work.backend=ComputeBackend(99);decoder.invalidate(5);decoder.submit(work);check(!waitFor(decoder,5).error.empty(),"Invalid backend escaped the worker error boundary.");
        work.generation=6;work.backend=ComputeBackend::Cpu;work.contentRevision=1;decoder.invalidate(6);decoder.submit(work);
        auto recovered=waitFor(decoder,6);check(recovered.error.empty()&&recovered.image->pixels==expected[0].pixels,"Playback did not recover after an error/content change.");retained=recovered.image;
        work.generation=7;work.frame=0;work.last=7;work.threads=6;work.decodeLanes=2;work.continuous=true;decoder.invalidate(7);decoder.submit(work);
        for(uint64_t n=0;n<8;++n){auto result=waitFor(decoder,7);check(result.error.empty()&&result.image&&result.image->pixels==expected[n].pixels,"Parallel playback changed ordered pixels.");
            check(result.throughputMs>0&&result.throughputFrames>0&&result.throughputFrames<=32,"Playback lost its measured throughput.");
            check(result.lanes==(n?2u:1u),"Parallel playback did not retain the direct first-frame path.");}
        work.generation=8;work.frame=3;work.continuous=false;decoder.invalidate(8);decoder.submit(work);
        auto direct=waitFor(decoder,8);check(direct.error.empty()&&direct.lanes==1&&direct.image->pixels==expected[3].pixels,"Seek did not leave the parallel window correctly.");
        check(direct.throughputFrames==1,"Seek retained previous playback throughput.");
        work.generation=9;work.frame=0;work.continuous=true;work.memoryMiB=256;decoder.invalidate(9);decoder.submit(work);
        for(uint64_t n=0;n<8;++n){auto result=waitFor(decoder,9);check(result.error.empty()&&result.lanes==1&&result.image->pixels==expected[n].pixels,"Low-memory playback did not use a safe serial fallback.");}
        work.generation=10;work.memoryMiB=0;decoder.invalidate(10);decoder.submit(work);
        auto first=waitFor(decoder,10);check(first.image&&first.image->pixels==expected[0].pixels,"Parallel playback restart failed.");
        work.generation=11;work.frame=2;work.continuous=false;decoder.invalidate(11);decoder.submit(work);
        auto cancelled=waitFor(decoder,11);check(cancelled.error.empty()&&cancelled.image->pixels==expected[2].pixels,"Cancelled parallel generation overwrote a seek result.");
        work.generation=12;work.frame=0;work.step=4;work.continuous=true;decoder.invalidate(12);decoder.submit(work);
        for(uint64_t n:{0,4,7}){auto result=waitFor(decoder,12);check(result.error.empty()&&result.frame==n&&result.image->pixels==expected[n].pixels,"Parallel stride playback lost the final frame.");}
        decoder.shutdown();decoder.shutdown();
    }
    check(retained&&retained->pixels==expected[0].pixels,"Closing playback invalidated a displayed frame.");
    std::filesystem::remove(path);
    std::cout<<"Playback PASS: ordered bounded prefetch, invalidation/seek, bounds, error recovery, content revision, retained image lifetime, shutdown\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
