#include "fajk/pipeline.hpp"
#include "fajk/media.hpp"
#include "fajk/compute.hpp"
#include <windows.h>
#include <iostream>
#include <string>

using namespace fajk;
namespace {
uint64_t cpuTime(){FILETIME created{},ended{},kernel{},user{};require(GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user)!=FALSE,"Cannot read CPU work.");
    ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return k.QuadPart+u.QuadPart;}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc>=7&&argc<=11,"motion_pipeline_probe SOURCE OUTPUT FRAMES BUDGET cpu|gpu serial|overlap [WORKERS=10] [TOLERANCE=14] [adaptive|stable] [fixed|borrow]");
    const std::wstring backend=argv[5],schedule=argv[6];require((backend==L"cpu"||backend==L"gpu")&&(schedule==L"serial"||schedule==L"overlap"),"Invalid probe mode.");
    MediaRuntime runtime;ConvertOptions options;options.replace=true;options.temporal=true;options.maxCompressionTemporal=true;
    options.maxFrameCount=std::stoull(argv[3]);options.encode.targetBytes=std::stoul(argv[4]);options.encode.backend=backend==L"gpu"?ComputeBackend::Gpu:ComputeBackend::Cpu;
    options.encode.speed=EncodeSpeed::Fast;options.encode.adaptiveQuantizationEnabled=false;options.encode.reconstruction441.protectText=false;
    options.workers=argc>7?std::stoul(argv[7]):10;options.similarTolerance=argc>8?std::stoul(argv[8]):14;options.overlapMotionPreparation=schedule==L"overlap";
    if(argc>9){const std::wstring policy=argv[9];require(policy==L"stable"||policy==L"adaptive","Invalid rate policy.");options.stabilizeTemporalQuantizer=policy==L"stable";}
    if(argc>10){const std::wstring schedule=argv[10];require(schedule==L"fixed"||schedule==L"borrow","Invalid CPU sharing policy.");options.borrowMotionWorkers=schedule==L"borrow";}
    const auto before=cpuTime();const auto result=convert(argv[1],argv[2],options,{});const auto cpu=cpuTime()-before;const auto& gpu=result.gpu;
    require(result.frames==options.maxFrameCount,"Short probe source.");
    std::cout<<"{\"frames\":"<<result.frames<<",\"bytes\":"<<result.bytes<<",\"wall_ms\":"<<result.elapsed*1000<<",\"process_cpu_ms\":"<<cpu/10000.
             <<",\"workers\":"<<result.workers<<",\"preparation_workers\":"<<result.preparationWorkers
             <<",\"shared_workers\":"<<(result.sharedWorkers?"true":"false")
             <<",\"gpu_transforms\":"<<gpu.transforms<<",\"gpu_native_decodes\":"<<gpu.nativeDecodes
             <<",\"gpu_motion_reconstructions\":"<<gpu.motionReconstructions
             <<",\"gpu_uploaded_bytes\":"<<gpu.uploadedBytes<<",\"gpu_downloaded_bytes\":"<<gpu.downloadedBytes
             <<",\"gpu_buffer_allocations\":"<<gpu.bufferAllocations<<",\"gpu_allocated_bytes\":"<<gpu.allocatedBufferBytes
             <<",\"gpu_fallbacks\":"<<gpu.fallbacks<<"}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
