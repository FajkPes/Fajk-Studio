#include "final_sharpen.hpp"
#include "fajk/hardware.hpp"
#include "gpu_compute.hpp"
#include "timing.hpp"
namespace fajk {
bool hasAvx2();
void finalSharpen(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,uint32_t strength){
    require(pixels.size()==pixelBytes(width,height)&&strength<=200,"Invalid final sharpening options.");
    if(!strength)return;
    TimingTimeline timing;
    const auto availableWorkers=workerLimit(workers);
    const bool avx2=hasAvx2();
    // With two AVX2 workers the streamed pass is now faster than a separate
    // RGBA upload/readback. Resident upscale+sharpen stays on the GPU; this
    // choice only concerns an independent final pass over a host image.
    if((availableWorkers==1||!avx2)&&gpuFinalSharpen(pixels,width,height,strength)){timing.mark("decode.final_sharpen.gpu");return;}
    const auto rowBytes=size_t(width)*4;
    const uint32_t rowsPerJob=std::clamp(((height/(2*availableWorkers))+7)&~7u,16u,64u);
    const auto jobs=(height+rowsPerJob-1)/rowsPerJob;
    auto prepare=[&](const uint8_t* input,uint8_t* output){if(avx2)finalSharpenLumaRowAvx2(input,output,width);else std::memcpy(output,input,rowBytes);};
    // Freeze only each band's two outside rows before any output is written.
    // A private three-row ring preserves original inside luma/alpha (RGBA on the
    // scalar fallback). Adjacent jobs
    // never read a row another job may have sharpened already.
    const auto scratchBytes=size_t(jobs)*5*rowBytes;
    thread_local Bytes cached;Bytes large;auto& scratch=scratchBytes<=8*1024*1024?cached:large;
    scratch.resize(scratchBytes); // All storage is acquired before output changes.
    for(uint32_t job=0;job<jobs;++job){const auto first=job*rowsPerJob,last=std::min(height,first+rowsPerJob);
        auto* halo=scratch.data()+size_t(job)*5*rowBytes;
        prepare(pixels.data()+size_t(first?first-1:0)*rowBytes,halo);
        prepare(pixels.data()+size_t(std::min(last,height-1))*rowBytes,halo+rowBytes);}
    auto* storage=scratch.data();
    parallelFor(jobs,workers,[&](uint32_t job){const uint32_t first=job*rowsPerJob,last=std::min(height,first+rowsPerJob);
        const auto* halo=storage+size_t(job)*5*rowBytes;
        auto* up=storage+(size_t(job)*5+2)*rowBytes;auto* row=up+rowBytes;auto* down=row+rowBytes;
        std::memcpy(up,halo,rowBytes);prepare(pixels.data()+size_t(first)*rowBytes,row);
        if(first+1<last)prepare(pixels.data()+size_t(first+1)*rowBytes,down);else std::memcpy(down,halo+rowBytes,rowBytes);
        for(uint32_t y=first;y<last;++y){auto* output=pixels.data()+size_t(y)*rowBytes;
            if(avx2)finalSharpenRowAvx2(output,row,up,down,output,width,strength);
            else for(uint32_t x=0;x<width;++x){const auto value=finalSharpRowPixel(row,up,down,width,x,strength);std::memcpy(output+size_t(x)*4,&value,4);}
            if(y+1<last){auto* spare=up;up=row;row=down;down=spare;
                if(y+2<last)prepare(pixels.data()+size_t(y+2)*rowBytes,down);else std::memcpy(down,halo+rowBytes,rowBytes);}
        }
    });
    timing.mark("decode.final_sharpen.cpu");
}
}
