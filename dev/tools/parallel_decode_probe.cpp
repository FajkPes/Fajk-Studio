#include "../src/sequence_decode_ahead.hpp"
#include "../src/gpu_activity_delta.hpp"
#include "fajk/container.hpp"
#include "fajk/codec.hpp"
#include <windows.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>

using namespace fajk;
namespace {
uint64_t cpuTime(){FILETIME created{},ended{},kernel{},user{};require(GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user)!=FALSE,"Cannot read CPU work.");
    ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return k.QuadPart+u.QuadPart;}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc>=5&&argc<=12,"parallel_decode_probe VIDEO OUTPUT cpu|gpu LANES [WORKERS=10] [WINDOW=5] [CHUNK=2] [HASH=0] [SHARP=50] [COUNT=0] [BORROW=0]");
    const std::wstring mode=argv[3];require(mode==L"cpu"||mode==L"gpu","Invalid backend.");
    const uint32_t lanes=std::stoul(argv[4]),workers=argc>5?std::stoul(argv[5]):10,window=argc>6?std::stoul(argv[6]):5,chunk=argc>7?std::stoul(argv[7]):2;
    const bool hash=argc>8&&std::stoul(argv[8])!=0;const auto sharp=argc>9?std::stoul(argv[9]):50;
    SequenceReader reader(argv[1]);const auto info=reader.info();const uint64_t requested=argc>10?std::stoull(argv[10]):0,count=requested?std::min(requested,info.count):info.count;
    require(count>0,"Empty sequence.");
    SequenceDecodeSettings settings;settings.path=argv[1];settings.width=info.width;settings.height=info.height;settings.generation=1;
    settings.threads=workers;settings.backend=mode==L"gpu"?ComputeBackend::Gpu:ComputeBackend::Cpu;settings.sharpening=uint32_t(sharp);
    settings.borrowCpuWorkers=argc>11&&std::stoul(argv[11])!=0;
    {auto warm=makeCodec(CodecId::Fajk);warm->setThreads(workers);warm->setComputeBackend(settings.backend);warm->setSharpening(settings.sharpening);
        for(uint64_t n=0;n<std::min<uint64_t>(16,count);++n)warm->decode(reader.read(n),info.width,info.height);}
    SequenceDecodeAhead ahead(lanes);GpuActivity gpu;std::vector<double> latencies;latencies.reserve(size_t(count));
    struct Signature {int64_t pts,duration;uint32_t hash;};std::vector<Signature> signatures;signatures.reserve(size_t(count));
    const auto cpuBefore=cpuTime();const auto start=std::chrono::steady_clock::now();
    ahead.start(settings,0,count-1,1,window,chunk);
    for(uint64_t n=0;n<count;++n){auto result=ahead.take(n,1);require(bool(result)&&result->error.empty(),result?result->error.c_str():"Cancelled decode.");
        require(result->frame==n&&result->image&&result->image->pixels.size()==pixelBytes(info.width,info.height),"Invalid decoded frame.");
        require(result->image->pts==reader.index()[n].pts&&result->image->duration==reader.index()[n].duration,"Timestamp differs.");
        latencies.push_back(result->decodeMs);accumulateGpu(gpu,result->gpu);
        if(hash)signatures.push_back({result->image->pts,result->image->duration,crc32(result->image->pixels)});
    }
    const auto wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),cpu=(cpuTime()-cpuBefore)/10000.;
    std::sort(latencies.begin(),latencies.end());
    std::ofstream output{Path(argv[2])};require(bool(output),"Cannot create decode result.");
    output<<std::setprecision(12)<<"{\"frames\":"<<count<<",\"lanes\":"<<lanes<<",\"workers\":"<<workers<<",\"window\":"<<window<<",\"chunk\":"<<chunk
          <<",\"hash_enabled\":"<<(hash?"true":"false")<<",\"borrow_cpu_workers\":"<<(settings.borrowCpuWorkers?"true":"false")<<",\"sharpening\":"<<sharp<<",\"wall_ms\":"<<wall<<",\"throughput_ms\":"<<wall/count<<",\"process_cpu_ms\":"<<cpu
          <<",\"mean_worker_decode_ms\":"<<std::accumulate(latencies.begin(),latencies.end(),0.)/count<<",\"p95_worker_decode_ms\":"<<latencies[size_t((count-1)*.95)]
          <<",\"gpu_uploaded_bytes\":"<<gpu.uploadedBytes<<",\"gpu_downloaded_bytes\":"<<gpu.downloadedBytes<<",\"gpu_native_decodes\":"<<gpu.nativeDecodes
          <<",\"gpu_buffer_allocations\":"<<gpu.bufferAllocations<<",\"gpu_allocated_bytes\":"<<gpu.allocatedBufferBytes
          <<",\"gpu_motion_reconstructions\":"<<gpu.motionReconstructions<<",\"gpu_failures\":"<<gpu.fallbacks<<",\"signatures\":[";
    for(size_t n=0;n<signatures.size();++n){const auto& row=signatures[n];if(n)output<<',';output<<'['<<row.pts<<','<<row.duration<<','<<row.hash<<']';}
    output<<"]}\n";require(bool(output),"Cannot write decode result.");
    std::cout<<"PASS lanes="<<lanes<<" window="<<window<<" chunk="<<chunk<<" frames="<<count<<" throughput="<<wall/count<<" ms/frame CPU="<<cpu/count<<" ms/frame hash="<<hash<<'\n';
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
