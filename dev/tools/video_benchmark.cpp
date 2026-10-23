// Real MP4 -> R470 export plus full sequential decode. Timing excludes PNG
// diagnostics and cross-backend comparisons, never GPU transfers or waits.
#include "fajk/media.hpp"
#include "fajk/pipeline.hpp"
#include "fajk/container.hpp"
#include "fajk/codec.hpp"
#include "fajk/compute.hpp"
#include "fajk/hardware.hpp"
#include "../src/decode_internal.hpp"
#include "../src/reconstruction441.hpp"
#include "../src/timing.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <chrono>
#include <set>
using namespace fajk;
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
struct Work {uint64_t time=0,cycles=0;};
Work work(){FILETIME a,b,k,u;Work result;
    if(GetProcessTimes(GetCurrentProcess(),&a,&b,&k,&u)){ULARGE_INTEGER x{},y{};x.LowPart=k.dwLowDateTime;x.HighPart=k.dwHighDateTime;y.LowPart=u.dwLowDateTime;y.HighPart=u.dwHighDateTime;result.time=x.QuadPart+y.QuadPart;}
    QueryProcessCycleTime(GetCurrentProcess(),&result.cycles);return result;
}
struct Sample {double encode=0,decode=0,read=0,encodeCpu=0,decodeCpu=0;GpuActivity enc{},dec{};};
GpuActivity difference(const GpuActivity& a,const GpuActivity& b){return {a.transforms-b.transforms,a.resamples-b.resamples,a.uploadedBytes-b.uploadedBytes,a.downloadedBytes-b.downloadedBytes,a.fallbacks-b.fallbacks,a.analyses-b.analyses,a.nativeDecodes-b.nativeDecodes,a.contourFilters-b.contourFilters};}
int decodeSequence(const Path& input,const Path& prefix,uint32_t workers,ComputeBackend backend,const Path& sourceInput={}){
    MediaRuntime runtime;std::unique_ptr<MediaSource> source;if(!sourceInput.empty())source=std::make_unique<MediaSource>(sourceInput);
    SequenceReader reader(input);const auto info=reader.info();require(info.count>1,"Expected a video sequence.");
    Frame decoded{info.width,info.height,0,1,Bytes(pixelBytes(info.width,info.height))},other=decoded;
    auto decode=[&](const Packet& packet,Frame& frame,ComputeBackend selected){decodeFrameToBuffer(packet,info.width,info.height,workers,100,frame.pixels,0,true,selected);};
    for(int i=0;i<2;++i)decode(reader.read(0),decoded,backend);
    const auto cpu=work();const auto activity=gpuActivity();const auto start=Clock::now();
    for(uint64_t i=0;i<info.count;++i)decode(reader.read(i),decoded,backend);
    const double elapsed=ms(start),cpuMs=(work().time-cpu.time)/10000.;const auto gpu=difference(gpuActivity(),activity);
    require(!gpu.fallbacks,"Sequential GPU decode fell back to CPU.");
    if(backend==ComputeBackend::Gpu)require(info.temporal?(gpu.nativeDecodes>0||uint64_t(info.width)*info.height<1024*1024):gpu.nativeDecodes==info.count,"Video did not use the expected GPU keyframe decodes.");
    // Every frame is compared after timing. This pass also checks timestamp
    // continuity and reuse of one output buffer across the entire video.
    const auto alternate=backend==ComputeBackend::Gpu?ComputeBackend::Cpu:ComputeBackend::Gpu;
    std::ofstream csv(prefix.wstring()+L".csv");require(bool(csv),"Cannot write sequence validation.");
    csv<<"frame,pts,duration,bytes,decoded_crc,cross_max,cross_mean,cross_fraction,cross_large_fraction\n";
    std::vector<uint32_t> checksums;checksums.reserve(size_t(info.count));int64_t end=0;uint64_t bytes=0;
    Frame original;int64_t sourceOrigin=0;
    for(uint64_t i=0;i<info.count;++i){const auto packet=reader.read(i);
        if(source){require(source->next(original)&&original.width==info.width&&original.height==info.height,"Full video source/frame mismatch.");if(!i)sourceOrigin=original.pts;require(original.pts-sourceOrigin==packet.pts,"Full video source timestamp mismatch.");}
        require(packet.pts==end&&packet.duration>0,"Video timestamp discontinuity.");end=packet.pts+packet.duration;bytes+=packet.data.size();
        decode(packet,decoded,backend);decode(packet,other,alternate);unsigned maximum=0;uint64_t sum=0,changed=0,large=0;
        for(size_t at=0;at<decoded.pixels.size();++at){const unsigned d=unsigned(std::abs(int(decoded.pixels[at])-other.pixels[at]));maximum=std::max(maximum,d);sum+=d;changed+=d!=0;large+=d>16;}
        const double mean=double(sum)/decoded.pixels.size(),fraction=double(changed)/decoded.pixels.size(),largeFraction=double(large)/decoded.pixels.size();
        require(mean<=.001&&fraction<=.002&&largeFraction<=.00001,"Full video cross-decode quality regression.");
        checksums.push_back(crc32(decoded.pixels));csv<<i<<','<<packet.pts<<','<<packet.duration<<','<<packet.data.size()<<','<<checksums.back()<<','<<maximum<<','<<mean<<','<<fraction<<','<<largeFraction<<'\n';
        if(i==0||i==info.count/2||i==info.count-1||maximum>16){const auto name=prefix.wstring()+L"-frame"+std::to_wstring(i);writePng(name+L".png",decoded);if(source)writePng(name+L"-source.png",original);if(maximum>16)writePng(name+L"-cross.png",other);}
    }
    if(source)require(!source->next(original),"Video export omitted source frames.");
    for(uint64_t i:{info.count-1,info.count/2,uint64_t(0)}){decode(reader.read(i),decoded,backend);require(crc32(decoded.pixels)==checksums[size_t(i)],"Video seek changed decoded pixels.");}
    std::ofstream summary(prefix.wstring()+L"-summary.csv");require(bool(summary),"Cannot write sequence measurement.");
    summary<<"frames,width,height,fps_num,fps_den,duration,workers,decode_ms,decode_cpu_ms,bytes,gpu_decodes,gpu_fallbacks\n"<<info.count<<','<<info.width<<','<<info.height<<','<<info.fpsNum<<','<<info.fpsDen<<','<<end<<','<<workers<<','<<elapsed<<','<<cpuMs<<','<<bytes<<','<<gpu.nativeDecodes<<','<<gpu.fallbacks<<'\n';
    std::cout<<"PASS "<<info.count<<" frames; full sequential decode "<<elapsed<<" ms; every frame cross-decoded; timestamps and seeking valid.\n";return 0;
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==5&&std::wstring(argv[1])==L"--diagnose"){
        SequenceReader reader(argv[2]);const auto packet=reader.read(std::stoull(argv[3]));const auto info=reader.info();const auto view=read441(packet.data,info.width,info.height);
        for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu})for(bool skip:{true,false}){
            ScopedComputeBackend compute(backend);DeferredUpscale deferred;deferred.skipBaseCleanup=skip;deferred.protectText=!skip;
            auto frame=decodeNestedFrame({view.child,packet.pts,packet.duration},info.width,info.height,10,{},&deferred);
            writePng(std::wstring(argv[4])+(backend==ComputeBackend::Gpu?L"-gpu":L"-cpu")+(skip?L"-raw.png":L"-base.png"),frame);
            std::cout<<(backend==ComputeBackend::Gpu?"gpu":"cpu")<<" skip "<<skip<<" working "<<frame.width<<'x'<<frame.height<<" text ranges "<<deferred.protectedText.ranges.size()<<'\n';
        }return 0;
    }
    if((argc==6||argc==7)&&std::wstring(argv[1])==L"--decode"){
        const std::wstring backend=argv[5];require(backend==L"cpu"||backend==L"gpu","Invalid backend.");
        const uint32_t workers=std::stoul(argv[4]);require(workers>0,"Invalid worker budget.");
        return decodeSequence(argv[2],argv[3],workers,backend==L"gpu"?ComputeBackend::Gpu:ComputeBackend::Cpu,argc==7?Path(argv[6]):Path{});
    }
    require(argc==8||argc==9,"video_benchmark INPUT PREFIX FRAMES WORKERS PACKET_BYTES cpu|gpu MAX_WIDTH(0=native) [fast|quality]");
    const Path input=argv[1],prefix=argv[2];const uint32_t count=std::stoul(argv[3]),workers=std::stoul(argv[4]),budget=std::stoul(argv[5]),maxWidth=std::stoul(argv[7]);
    const std::wstring backendName=argv[6];require(backendName==L"cpu"||backendName==L"gpu","Invalid backend.");
    const auto backend=backendName==L"gpu"?ComputeBackend::Gpu:ComputeBackend::Cpu;
    require(count>=2&&count<=512&&workers>0&&budget>=1024,"Invalid video measurement options.");
    MediaRuntime runtime;MediaSource source(input,maxWidth);const auto info=source.info();require(!info.image,"Video benchmark requires a real video.");
    require(uint64_t(pixelBytes(info.width,info.height))*count<=2ull*1024*1024*1024,"Benchmark frame cache exceeds 2 GiB; reduce frames/width.");
    std::vector<Frame> frames;frames.reserve(count);Frame next;int64_t origin=0;
    for(uint32_t i=0;i<=count;++i){if(!source.next(next))break;if(i==0)origin=next.pts;next.pts-=origin;
        if(!frames.empty())frames.back().duration=next.pts-frames.back().pts;
        if(i==count)break;frames.push_back(std::move(next));}
    require(frames.size()==count,"Video does not contain the requested number of frames.");
    std::set<uint32_t> sources;for(const auto& frame:frames)sources.insert(crc32(frame.pixels));require(sources.size()>1,"Video benchmark received repeated identical frames.");
    const auto n=uint32_t(frames.size());EncodeOptions options;options.backend=backend;options.threads=workers;options.quality=100;options.targetBytes=budget;
    if(argc==9){const std::wstring mode=argv[8];require(mode==L"fast"||mode==L"quality","Invalid speed mode.");options.speed=mode==L"fast"?EncodeSpeed::Fast:EncodeSpeed::MoreQuality;}
    auto codec=makeCodec(CodecId::Fajk);codec->setComputeBackend(backend);codec->setThreads(workers);
    Packet warm;for(int i=0;i<2;++i)warm=codec->encode(frames[0],options);
    std::vector<Packet> packets(n);std::vector<Sample> samples(n);
    std::ofstream stages(prefix.wstring()+L"-stages.csv");require(bool(stages),"Cannot write stage profile.");stages<<"frame,stage,ms\n";
    for(uint32_t i=0;i<n;++i){std::vector<TimingEvent> events;TimingCapture capture(&events);const auto before=gpuActivity();const auto cpu=work();const auto start=Clock::now();
        packets[i]=codec->encode(frames[i],options);samples[i].encode=ms(start);samples[i].encodeCpu=(work().time-cpu.time)/10000.;samples[i].enc=difference(gpuActivity(),before);
        require(packets[i].data.size()<=budget,"Video packet exceeded its budget.");
        for(const auto& event:events)stages<<i<<','<<event.stage<<','<<event.ms<<'\n';
    }
    SequenceInfo sequence{info.width,info.height,info.fpsNum,info.fpsDen,CodecId::Fajk,1,0,0};
    const Path direct=prefix.wstring()+L"-codec.fajkv",exported=prefix.wstring()+L"-pipeline.fajkv";
    {SequenceWriter writer(direct,sequence,true);for(const auto& packet:packets)writer.append(packet);writer.finish();}
    ConvertOptions conversion;conversion.encode=options;conversion.maxWidth=maxWidth;conversion.workers=workers;conversion.maxFrameCount=n;conversion.replace=true;
    const auto exportCpu=work();const auto exportStart=Clock::now();const auto progress=convert(input,exported,conversion,{});const double exportMs=ms(exportStart),exportCpuMs=(work().time-exportCpu.time)/10000.;
    require(progress.frames==n,"Video export frame count differs.");
    SequenceReader reader(exported);require(reader.info().count==n&&reader.info().width==info.width&&reader.info().height==info.height,"Video export metadata differs.");
    for(uint32_t i=0;i<n;++i){const auto packet=reader.read(i);require(packet.data==packets[i].data&&packet.pts==packets[i].pts&&packet.duration==packets[i].duration,"Pipeline changed a frame, timestamp, order or backend output.");}
    // Cancel while frame jobs are in flight. A replaced destination must retain
    // its complete previous contents and temporary transactions must disappear.
    const Path cancelledFile=prefix.wstring()+L"-cancelled.fajkv";
    std::filesystem::copy_file(exported,cancelledFile,std::filesystem::copy_options::overwrite_existing);
    std::stop_source stop;bool cancelled=false;
    try{convert(input,cancelledFile,conversion,stop.get_token(),[&](const Progress& p){if(p.frames)stop.request_stop();});}
    catch(const std::exception&){cancelled=stop.stop_requested();}
    require(cancelled,"Video cancellation did not interrupt conversion.");
    {SequenceReader preserved(cancelledFile);require(preserved.info().count==n,"Cancellation replaced the destination.");
        for(uint32_t i=0;i<n;++i)require(preserved.read(i).data==packets[i].data,"Cancellation changed a destination packet.");}
    for(const auto& entry:std::filesystem::directory_iterator(cancelledFile.parent_path()))
        require(entry.path().filename().wstring().find(cancelledFile.filename().wstring()+L".")!=0,"Cancellation leaked a temporary transaction.");
    std::filesystem::remove(cancelledFile);
    // Exercise the one-frame path and the conservative RAM guard using the
    // same real source. They must preserve serial encoding and timestamps.
    const Path guardedFile=prefix.wstring()+L"-guarded.fajkv";
    for(uint32_t guardedCount:{1u,std::min(4u,n)}){
        auto guarded=conversion;guarded.maxFrameCount=guardedCount;
        if(guardedCount>1)guarded.memoryMiB=1024;
        convert(input,guardedFile,guarded,{},{});
        {SequenceReader guardedReader(guardedFile);require(guardedReader.info().count==guardedCount,"Guarded video frame count changed.");
            for(uint32_t i=0;i<guardedCount;++i){const auto p=guardedReader.read(i);require(p.data==packets[i].data&&p.pts==packets[i].pts&&p.duration==packets[i].duration,"RAM guard or single frame export changed encoding.");}}
    }
    std::filesystem::remove(guardedFile);
    Frame decoded{info.width,info.height,0,1,Bytes(pixelBytes(info.width,info.height))};
    for(int i=0;i<2;++i)decodeFrameToBuffer(warm,info.width,info.height,workers,100,decoded.pixels,0,true,backend);
    std::vector<uint32_t> outputCrc(n);
    for(uint32_t i=0;i<n;++i){const auto readStart=Clock::now();const auto packet=reader.read(i);samples[i].read=ms(readStart);
        std::vector<TimingEvent> events;TimingCapture capture(&events);const auto before=gpuActivity();const auto cpu=work();const auto start=Clock::now();
        decodeFrameToBuffer(packet,info.width,info.height,workers,100,decoded.pixels,0,true,backend);samples[i].decode=ms(start);samples[i].decodeCpu=(work().time-cpu.time)/10000.;samples[i].dec=difference(gpuActivity(),before);
        // Hashes are timed separately from codec decode (and reported below).
        outputCrc[i]=crc32(decoded.pixels);
        for(const auto& event:events)stages<<i<<','<<event.stage<<','<<event.ms<<'\n';
    }
    // Measure the actual sequential reader/decoder again without image hashes,
    // profiling or diagnostic IO. CRC of raw RGBA is intentionally not part of
    // production playback and would otherwise dominate this benchmark.
    const auto playbackCpu=work();const auto playbackStart=Clock::now();
    for(uint32_t i=0;i<n;++i){const auto packet=reader.read(i);decodeFrameToBuffer(packet,info.width,info.height,workers,100,decoded.pixels,0,true,backend);}
    const double decodeTotalMs=ms(playbackStart),decodeCpuMs=(work().time-playbackCpu.time)/10000.;
    std::ofstream csv(prefix.wstring()+L".csv");require(bool(csv),"Cannot write video measurement.");
    csv<<"frame,pts,duration,source_crc,decoded_crc,bytes,encode_ms,decode_ms,read_ms,encode_cpu_ms,decode_cpu_ms,enc_gpu_transforms,enc_gpu_resamples,enc_gpu_analyses,enc_upload,enc_download,dec_gpu_transforms,dec_gpu_contours,dec_upload,dec_download,gpu_fallbacks,cross_max,cross_mean,cross_fraction,cross_large_fraction\n";
    Frame other=decoded;const auto alternate=backend==ComputeBackend::Gpu?ComputeBackend::Cpu:ComputeBackend::Gpu;
    for(uint32_t i=0;i<n;++i){decodeFrameToBuffer(packets[i],info.width,info.height,workers,100,decoded.pixels,0,true,backend);
        require(crc32(decoded.pixels)==outputCrc[i],"Video decode changed on reuse.");
        decodeFrameToBuffer(packets[i],info.width,info.height,workers,100,other.pixels,0,true,alternate);
        unsigned maximum=0;uint64_t sum=0,changed=0,large=0;for(size_t at=0;at<decoded.pixels.size();++at){const unsigned d=unsigned(std::abs(int(decoded.pixels[at])-other.pixels[at]));maximum=std::max(maximum,d);sum+=d;changed+=d!=0;large+=d>16;}
        const double mean=double(sum)/decoded.pixels.size(),fraction=double(changed)/decoded.pixels.size(),largeFraction=double(large)/decoded.pixels.size();
        const auto& s=samples[i];csv<<i<<','<<packets[i].pts<<','<<packets[i].duration<<','<<crc32(frames[i].pixels)<<','<<outputCrc[i]<<','<<packets[i].data.size()<<','<<s.encode<<','<<s.decode<<','<<s.read<<','<<s.encodeCpu<<','<<s.decodeCpu<<','<<s.enc.transforms<<','<<s.enc.resamples<<','<<s.enc.analyses<<','<<s.enc.uploadedBytes<<','<<s.enc.downloadedBytes<<','<<s.dec.transforms<<','<<s.dec.contourFilters<<','<<s.dec.uploadedBytes<<','<<s.dec.downloadedBytes<<','<<s.enc.fallbacks+s.dec.fallbacks<<','<<maximum<<','<<mean<<','<<fraction<<','<<largeFraction<<'\n';
        if(i==0||i==n/2||i==n-1||maximum>16){const auto name=prefix.wstring()+L"-frame"+std::to_wstring(i);writePng(name+L"-source.png",frames[i]);writePng(name+L"-decoded.png",decoded);writePng(name+L"-cross.png",other);}
        require(mean<=.001&&fraction<=.002&&largeFraction<=.00001,"Video cross-decode quality regression.");
        require(!s.enc.fallbacks&&!s.dec.fallbacks,"Video GPU error fallback.");
        if(backend==ComputeBackend::Gpu)require(s.enc.resamples+s.enc.analyses+s.enc.transforms>0&&s.dec.transforms>0,"Video silently used CPU instead of GPU.");
    }
    std::ofstream summary(prefix.wstring()+L"-summary.csv");summary<<"frames,width,height,fps_num,fps_den,source_duration,unique_sources,workers,export_ms,export_cpu_ms,decode_ms,decode_cpu_ms,bytes\n"<<n<<','<<info.width<<','<<info.height<<','<<info.fpsNum<<','<<info.fpsDen<<','<<info.duration<<','<<sources.size()<<','<<workers<<','<<exportMs<<','<<exportCpuMs<<','<<decodeTotalMs<<','<<decodeCpuMs<<','<<progress.bytes<<'\n';
    std::cout<<"PASS "<<n<<" moving frames "<<info.width<<'x'<<info.height<<"; MP4 export "<<exportMs<<" ms; sequential decode "<<decodeTotalMs<<" ms; exact pipeline packets/timestamps.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
