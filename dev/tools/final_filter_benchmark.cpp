#include "fajk/codec.hpp"
#include "fajk/compute.hpp"
#include "../src/final_sharpen.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace fajk;
int wmain(int argc,wchar_t** argv){try{
    require(argc==3,"final_filter_benchmark INPUT.png OUTPUT.csv");const auto source=readImage(argv[1]);
    std::ofstream out(argv[2]);out<<"backend,workers,strength,repeat,ms,gpu_sharpens,gpu_fallbacks\n";
    for(uint32_t workers:{1u,2u,10u})for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu}){
        ScopedComputeBackend selected(backend);auto pixels=source.pixels;
        for(int n=0;n<2;++n){pixels=source.pixels;finalSharpen(pixels,source.width,source.height,workers,100);}
        for(int n=0;n<16;++n)for(uint32_t strength:{0u,50u,100u}){
            pixels=source.pixels;const auto before=gpuActivity();const auto start=std::chrono::steady_clock::now();
            finalSharpen(pixels,source.width,source.height,workers,strength);
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            require(gpuActivity().fallbacks==before.fallbacks,"GPU sharpening error fallback.");
            out<<(backend==ComputeBackend::Gpu?"gpu":"cpu")<<','<<workers<<','<<strength<<','<<n<<','<<ms<<','<<gpuActivity().sharpens-before.sharpens<<','<<gpuActivity().fallbacks-before.fallbacks<<'\n';
        }
    }
    std::cout<<"PASS final filter timings include internal snapshot, transfers, dispatch and waiting.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
