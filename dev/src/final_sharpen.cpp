#include "final_sharpen.hpp"
#include "fajk/hardware.hpp"
#include "gpu_compute.hpp"
#include "timing.hpp"
namespace fajk {
bool hasAvx2();
void finalSharpen(std::span<uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,uint32_t strength){
    require(pixels.size()==pixelBytes(width,height)&&strength<=100,"Invalid final sharpening options.");
    if(!strength)return;
    TimingTimeline timing;
    if(workers<=2&&gpuFinalSharpen(pixels,width,height,strength)){timing.mark("decode.final_sharpen.gpu");return;}
    thread_local Bytes cached;Bytes large;auto& snapshot=pixels.size()<=64*1024*1024?cached:large;
    snapshot.assign(pixels.begin(),pixels.end());
    const bool avx2=hasAvx2();
    parallelFor((height+15)/16,workers,[&](uint32_t job){const uint32_t first=job*16,last=std::min(height,first+16);
        if(avx2)finalSharpenRowsAvx2(snapshot,pixels,width,height,first,last,strength);
        else for(uint32_t y=first;y<last;++y)for(uint32_t x=0;x<width;++x){const auto value=finalSharpPixel(snapshot,width,height,x,y,strength);std::memcpy(pixels.data()+(size_t(y)*width+x)*4,&value,4);}
    });
    timing.mark("decode.final_sharpen.cpu");
}
}
