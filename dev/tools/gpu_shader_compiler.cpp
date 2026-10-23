#include "../src/gpu_shaders.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
// Compile current R470 kernels at build time. Starting a codec must not compile
// HLSL, write shader caches, or require a compiler DLL in the decode SDK.
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 1;
    try{
        const std::array names{"horizontal","vertical","resampleH","resampleV","sourceAnalysis","cellMeans","chromaGuides","nativeColor","sourceColor","coarseH","coarseV","nativeRestore","inverseHorizontal","inverseVertical","contourH","contourV","contourOutput","contourWeak","edgeUpscale","colorModelStatistics","colorModelPatches","colorDetailMoments","sourceWavelet","finalSharpen"};
        std::ofstream out(std::filesystem::path(argv[1]),std::ios::binary|std::ios::trunc);
        if(!out)throw std::runtime_error("Cannot create GPU kernel header.");
        out<<"// Generated from src/gpu_shaders.hpp. Do not edit.\n#pragma once\n#include <array>\n#include <cstddef>\nnamespace fajk {\n";
        for(size_t i=0;i<names.size();++i){
            Microsoft::WRL::ComPtr<ID3DBlob> code,errors;
            const auto hr=D3DCompile(fajk::gpuComputeShaders,sizeof(fajk::gpuComputeShaders)-1,"R470 GPU",nullptr,nullptr,names[i],"cs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&errors);
            if(FAILED(hr))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"GPU kernel compilation failed.");
            out<<"inline constexpr unsigned char gpuKernel"<<i<<"[]={";
            const auto* bytes=static_cast<const unsigned char*>(code->GetBufferPointer());
            for(size_t j=0;j<code->GetBufferSize();++j){if(j%32==0)out<<'\n';out<<unsigned(bytes[j])<<',';}
            out<<"\n};\n";
        }
        out<<"struct GpuKernel {const void* bytes;size_t size;};\ninline constexpr std::array gpuKernels{\n";
        for(size_t i=0;i<names.size();++i)out<<"GpuKernel{gpuKernel"<<i<<",sizeof(gpuKernel"<<i<<")},\n";
        out<<"};\n}\n";out.flush();if(!out)throw std::runtime_error("Cannot write GPU kernels.");
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
