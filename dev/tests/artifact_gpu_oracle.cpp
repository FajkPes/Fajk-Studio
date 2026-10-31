#include "../src/gpu_compute.hpp"
#include "fajk/hardware.hpp"
#include "../src/timing.hpp"
#include <immintrin.h>
#include <cstring>
#include <iostream>
#include "../src/edge_model_avx2.hpp"
#include "../src/artifact_cleanup.hpp"
#include "../src/fast_reconstruction.hpp"

namespace fajk {
bool hasAvx2();
bool nativeReciprocalTableAvx2(std::array<uint32_t,4096>&);
void verifyArtifactGpu(){
    if(!hasAvx2())return;
    std::array<uint32_t,4096> table;
    if(!nativeReciprocalTableAvx2(table)){std::cout<<"SKIP optional exact artifact reciprocal table\n";return;}
    uint64_t checks=0,changed=0;
    for(auto [width,height]:{std::pair{1u,1u},std::pair{7u,9u},std::pair{65u,37u},std::pair{641u,385u}})
    for(unsigned pattern=0;pattern<3;++pattern){
        Frame input{width,height,0,1,Bytes(pixelBytes(width,height))};
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
            const auto at=(size_t(y)*width+x)*4;
            const double distance=double(x)-width*.5+(double(y)-height*.5)*.37;
            const double t=std::clamp(.5+distance/(pattern==0?3.:7.),0.,1.);
            for(unsigned c=0;c<3;++c){const int a[]={30,70,170},b[]={210,120,35};
                const double ringing=pattern==1?std::sin(distance*.9)*6*std::exp(-std::abs(distance)/12):0;
                const double value=pattern==2?110+std::sin(x*.03+c)*25+std::cos(y*.02+c)*20:(a[c]+(b[c]-a[c])*t+ringing*(c==2?-1:1));
                input.pixels[at+c]=uint8_t(std::clamp(int(value+.5),0,255));}
            input.pixels[at+3]=(x==width-1&&y==height-1&&width>1)?127:255;
        }
        for(auto tuning:{ContourTuning{},ContourTuning{50,50},ContourTuning{175,175}})
        for(uint32_t grid:{8u,16u})for(float contours:{.3f,.8f,2.f}){
            std::vector<edgeModel::Guide> guides;
            auto build=[&]<int spacing>(){edgeModel::buildGuides<spacing>(input.pixels.data(),width,height,4,0,contours,true,guides,tuning);};
            if(grid==8)build.template operator()<8>();else build.template operator()<16>();
            Frame expected=input;
            const auto columns=(width+grid-1)/grid,rows=(height+grid-1)/grid;
            for(uint32_t y=0;y<height;++y){
                if(grid==8)edgeModel::render<8,false>(expected.pixels.data()+size_t(y)*width*4,width,y,guides.data(),columns,rows,0,contours);
                else edgeModel::render<16,false>(expected.pixels.data()+size_t(y)*width*4,width,y,guides.data(),columns,rows,0,contours);
            }
            Bytes guarded(input.pixels.size()+32,197);std::copy(input.pixels.begin(),input.pixels.end(),guarded.begin()+16);
            ScopedComputeBackend backend(ComputeBackend::Gpu);
            static_assert(sizeof(edgeModel::Guide)==24*sizeof(float));
            const auto frozen=std::span(reinterpret_cast<const float*>(guides.data()),guides.size()*24);
            require(gpuArtifactCleanup(std::span(guarded).subspan(16,input.pixels.size()),width,height,grid,frozen,contours),"GPU artifact renderer declined supported oracle.");
            size_t mismatch=0,first=0;
            for(size_t i=0;i<input.pixels.size();++i){changed+=expected.pixels[i]!=input.pixels[i];if(guarded[i+16]!=expected.pixels[i]){if(!mismatch)first=i;++mismatch;}}
            if(mismatch){std::cerr<<"Artifact oracle mismatch w="<<width<<" h="<<height<<" grid="<<grid<<" pattern="<<pattern<<" contours="<<contours
                <<" bytes="<<mismatch<<" first="<<first<<" expected="<<int(expected.pixels[first])<<" actual="<<int(guarded[first+16])<<'\n';}
            require(mismatch==0,"GPU artifact renderer differs from CPU pixels.");
            for(unsigned n=0;n<16;++n)require(guarded[n]==197&&guarded[guarded.size()-1-n]==197,"GPU artifact renderer changed output guards.");
            ++checks;
        }
    }
    require(changed>0,"Artifact oracle never exercised a correction.");
    // A native-result ticket is optional. Prove both its resident hit and a
    // forced eviction preserve the complete output, even when other native
    // decodes repeatedly reuse one host address with different image contents.
    {
        ScopedComputeBackend backend(ComputeBackend::Gpu);
        std::array<std::vector<float>,3> coefficients;
        std::array<GpuPlane,3> planes;
        for(unsigned c=0;c<3;++c){
            coefficients[c].resize(512u*512u);
            for(unsigned i=0;i<64;++i)coefficients[c][i]=float(int(i%9)-4)*(c+1);
            planes[c]={coefficients[c],512,512};
        }
        Frame native{512,512,0,1,Bytes(pixelBytes(512,512))};
        const GpuNativeSettings settings{512,512};
        require(gpuDecodeNative(planes,settings,native.pixels),"Native ticket oracle declined.");
        auto render=[&](uint64_t stamp){
            auto plan=prepareArtifactGuidePlan(native.pixels,512,512,4,0,.8f,stamp);
            require(bool(plan),"Native ticket oracle has no artifact plan.");
            return fastReconstruct441(native,1024,1024,4,80000,{}, {},nullptr,nullptr,nullptr,plan.get());
        };
        const auto expected=render(0);
        GpuDecodedSource ticket;
        {ScopedGpuDecodedSource capture(&ticket);
            require(gpuDecodeNative(planes,settings,native.pixels),"Native ticket capture declined.");}
        require(ticket.stamp&&ticket.pixels==native.pixels.data()&&ticket.bytes==native.pixels.size(),"Native ticket does not identify the host image.");
        auto before=gpuActivity();const auto resident=render(ticket.stamp);
        const auto hitUploads=gpuActivity().uploadedBytes-before.uploadedBytes;
        require(resident.pixels==expected.pixels,"Resident native ticket changed pixels.");
        Frame scratch{512,512,0,1,Bytes(pixelBytes(512,512))};
        Bytes resized(pixelBytes(513,513));
        for(unsigned i=0;i<8;++i){
            coefficients[0][0]=float(100+i*50);
            GpuDecodedSource other;ScopedGpuDecodedSource capture(&other);
            require(gpuDecodeNative(planes,settings,scratch.pixels)&&other.stamp!=ticket.stamp,"Native eviction fixture failed.");
            require(gpuResample(scratch,513,513,resized),"Native eviction fixture did not switch the active source.");
        }
        before=gpuActivity();const auto evicted=render(ticket.stamp);
        const auto missUploads=gpuActivity().uploadedBytes-before.uploadedBytes;
        require(evicted.pixels==expected.pixels,"Evicted native ticket did not use the unchanged host image.");
        require(missUploads>=hitUploads+native.pixels.size(),"Native ticket oracle did not exercise both a cache hit and eviction upload.");
    }
    std::cout<<"GPU artifact exact oracle PASS "<<checks<<" configurations, corrected bytes="<<changed<<'\n';
}
}
