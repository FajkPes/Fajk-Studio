#include "fajk/container.hpp"
#include "fajk/compute.hpp"
#include "../src/decode_internal.hpp"
#include "../src/timing.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <string>
using namespace fajk;
int wmain(int argc,wchar_t** argv){try{
    require(argc>=4,"motion_decode_profile VIDEO OUTPUT.csv cpu|gpu [WORKERS]");
    const std::wstring backend=argv[3];require(backend==L"cpu"||backend==L"gpu","Invalid backend.");
    const auto compute=backend==L"gpu"?ComputeBackend::Gpu:ComputeBackend::Cpu;
    const uint32_t workers=argc>4?std::stoul(argv[4]):10;
    SequenceReader reader(argv[1]);const auto info=reader.info();
    Bytes output(pixelBytes(info.width,info.height));std::ofstream csv{Path(argv[2])};require(bool(csv),"Cannot create profile.");
    csv<<"frame,dependent,stage,ms\n";std::map<std::string,std::pair<double,uint64_t>> sums;
    for(uint64_t i=0;i<info.count;++i){const auto readStart=std::chrono::steady_clock::now();const auto packet=reader.read(i);
        const auto readMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-readStart).count();std::vector<TimingEvent> events;
        const auto start=std::chrono::steady_clock::now();
        {TimingCapture capture(&events);decodeFrameToBuffer(packet,info.width,info.height,workers,100,output,0,true,compute);}
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        events.push_back({"decode.total",elapsed});
        events.push_back({"decode.packet_read",readMs});
        events.push_back({"decode.end_to_end",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-readStart).count()});
        for(const auto& event:events){csv<<i<<','<<bool(packet.reference)<<','<<event.stage<<','<<event.ms<<'\n';
            auto& row=sums[event.stage];row.first+=event.ms;++row.second;}
    }
    require(bool(csv),"Cannot write profile.");
    for(const auto& [stage,value]:sums)std::cout<<stage<<": "<<value.first/value.second<<" ms ("<<value.second<<" calls)\n";
    if(compute==ComputeBackend::Gpu){const auto activity=gpuActivity();std::cout<<"GPU activity: uploaded="<<activity.uploadedBytes<<" downloaded="<<activity.downloadedBytes<<" resident_reconstructions="<<activity.motionReconstructions<<" sparse_coefficient_uploads="<<activity.sparseCoefficientUploads<<" fallbacks="<<activity.fallbacks<<'\n';std::cout<<"GPU allocation activity: count="<<activity.bufferAllocations<<" bytes="<<activity.allocatedBufferBytes<<'\n';}
    std::cout<<"PASS "<<info.count<<" frames. Nested stages overlap decode.total; inverse may overlap warp (GPU) or synthesis_total (CPU). Do not sum them.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
