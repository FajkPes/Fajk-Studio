#include "fajk/codec.hpp"
#include "../src/gpu_compute.hpp"
#include "../src/wavelet.hpp"
#include "../src/resample.hpp"
#include "../src/source_priority.hpp"
#include "../src/fast_reconstruction.hpp"
#include "../src/color_prediction_fit.hpp"
#include "../src/color_prediction_rows.hpp"
#include "../src/sparse_coefficients_avx2.hpp"
#include "../src/decode_presentation.hpp"
#include "../src/final_sharpen.hpp"
#include <iostream>
#include <random>
#include <future>
#include <cmath>
#include <cstring>
using namespace fajk;
namespace fajk {bool hasAvx2();void verifyArtifactGpu();void fastEdgeUpscaleRow(const uint8_t*,uint32_t,uint32_t,uint8_t*,uint32_t,const int*,const float*,int,float,float);}
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
int main(){try{
    if(hasAvx2())for(size_t count:{1u,7u,8u,9u,31u,128u,4097u}){
        std::vector<float> values(count);const uint32_t bits[]={0,0x80000000,0x3f800000,0xbf000000,0x00000001,0x7fc12345,0x7f800000};
        std::vector<SparseCoefficient> expected{{3,0x3f800000}},actual=expected;
        for(size_t i=0;i<count;++i){values[i]=std::bit_cast<float>(bits[i%7]);if(bits[i%7])expected.push_back({uint32_t(i+17),bits[i%7]});}
        check(appendSparseCoefficientsAvx2(values,17,expected.size(),actual)&&actual==expected,"Sparse AVX2 pack changed float bits, order or tail.");
        actual={{3,0x3f800000}};values.back()=1.f;check(!appendSparseCoefficientsAvx2(values,17,1,actual)&&actual.size()==1,"Sparse AVX2 pack exceeded its cap.");
    }
    // Native decode can be the first GPU operation in a fresh SDK process.
    // Do not warm the device with gpuInfo before exercising sparse upload.
    bool coldNative=false;
    {
        ScopedComputeBackend backend(ComputeBackend::Gpu);
        std::array<std::vector<float>,3> data;
        std::array<GpuPlane,3> planes;
        for(unsigned c=0;c<3;++c){data[c].resize(512u*512u);planes[c]={data[c],512,512};}
        Bytes output(pixelBytes(512,512));
        coldNative=gpuDecodeNative(planes,GpuNativeSettings{512,512},output);
    }
    const auto info=gpuInfo();
    if(!info.available){std::cout<<"SKIP GPU: "<<info.error<<'\n';return info.device.empty()?77:1;}
    verifyArtifactGpu();
    std::cout<<"GPU: "<<info.device<<'\n';
    check(coldNative,"Native sparse decode failed on a cold GPU device.");
    std::mt19937 rng(470);
    // Stored chroma cleanup must remain on-device, including odd borders.
    // Compare all three strengths against the established CPU recursion;
    // filtering off must also disable this stage, rather than hide a pass.
    for(auto [w,h]:{std::pair{513u,517u},std::pair{768u,512u}}){
        Frame source{w,h,0,1,Bytes(pixelBytes(w,h),255)};
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){auto* p=source.pixels.data()+(size_t(y)*w+x)*4;
            const int noise=int(rng()%17)-8;const bool edge=x>w/2;
            p[0]=uint8_t((edge?170:85)+noise);p[1]=uint8_t(120-noise/2);p[2]=uint8_t((edge?60:170)-noise);}
        EncodeOptions options;options.threads=3;options.targetBytes=6000;options.chroma420=true;options.adaptiveQuantizationEnabled=false;
        const auto original=encodeWavelet(source,options,false);
        check(original.data[4]==114,"Color cleanup test requires a native packet.");
        for(uint8_t mode:{1,2,3})for(uint32_t strength:{25u,100u}){
            auto packet=original;packet.data[24]=uint8_t(8|(mode<<6));
            const auto expected=decodeWavelet(packet,w,h,3,{},100,0,{},nullptr,false,{},32,false,0,strength);
            Frame actual;const auto before=gpuActivity().nativeDecodes;
            {ScopedComputeBackend backend(ComputeBackend::Gpu);actual=decodeWavelet(packet,w,h,3,{},100,0,{},nullptr,false,{},32,false,0,strength);}
            check(gpuActivity().nativeDecodes==before+1,"Color cleanup silently left the GPU native route.");
            double error=0;int maximum=0;for(size_t at=0;at<actual.pixels.size();++at){const int d=std::abs(int(actual.pixels[at])-expected.pixels[at]);error+=d;maximum=std::max(maximum,d);}
            check(maximum<=2&&error/actual.pixels.size()<.01,"GPU color cleanup disagrees with CPU recursion.");
            const auto disabled=decodeWavelet(packet,w,h,3,{},100,0,{},nullptr,false,{},32,false,0,0);
            packet.data[24]=8;
            check(decodeWavelet(packet,w,h,3,{},100,0,{},nullptr,false,{},32,false,0,0).pixels==disabled.pixels,"Disabled filtering still denoises color.");
        }
    }
    double worst=0;
    // Final sharpening consumes the resident upscale surface: exact former
    // GPU-upscale + CPU-sharpen output, with one final RGBA readback only.
    {
        Frame native{513,517,0,1,Bytes(pixelBytes(513,517),255)};
        for(size_t at=0;at<native.pixels.size();at+=4)for(unsigned c=0;c<3;++c)native.pixels[at+c]=uint8_t(rng());
        ScopedComputeBackend backend(ComputeBackend::Gpu);
        for(bool fixed:{false,true})for(uint32_t strength:{1u,50u,100u,150u,200u}){
            Reconstruction441 controls;controls.fixedIntensity=fixed;
            auto source=native;auto expected=fastReconstruct441(source,1025,1039,4,30000,controls);
            finalSharpen(expected.pixels,expected.width,expected.height,4,strength);
            DecodePresentation presentation{strength};source=native;const auto before=gpuActivity();
            auto actual=fastReconstruct441(source,1025,1039,4,30000,controls, {},nullptr,nullptr,&presentation);const auto after=gpuActivity();
            check(presentation.applied&&actual.pixels==expected.pixels,"Resident final sharpening changed pixels.");
            check(after.sharpens==before.sharpens+1&&after.downloadedBytes-before.downloadedBytes==actual.pixels.size(),"Resident sharpening added a readback or missed the GPU.");
        }
    }
    // Independent scalar reference for the integer in-loop color filter.
    // Exact equality includes signed rounding, alpha, borders and skip masks.
    for(auto dimensions:{std::array{512u,512u},std::array{513u,517u}})for(uint32_t step:{1u,4u,15u,16u,32u,127u,256u})for(bool adjacent:{false,true}){
        Frame source{dimensions[0],dimensions[1],0,1,Bytes(pixelBytes(dimensions[0],dimensions[1]))};
        for(size_t at=0;at<source.pixels.size();++at)source.pixels[at]=uint8_t(at<source.pixels.size()/2?rng():96+rng()%65);
        const auto cols=(source.width+7)/8;Bytes mask(size_t(cols)*((source.height+7)/8));for(auto& m:mask)m=uint8_t(rng()%4);
        auto expected=source;
        auto color=[&](uint32_t x,uint32_t y){const auto at=(size_t(y)*source.width+x)*4;
            const int r=source.pixels[at],g=source.pixels[at+1],b=source.pixels[at+2];return std::array{r+2*g+b,r-b,2*g-r-b};};
        auto divide=[](int value,int divisor){return value<0?-((-value+divisor/2)/divisor):(value+divisor/2)/divisor;};
        if(step>=4)for(uint32_t y=0;y<source.height;++y)for(uint32_t x=0;x<source.width;++x){
            if(mask[size_t(y/8)*cols+x/8]&1)continue;
            const auto center=color(x,y);const int range=int(std::clamp(step/2,2u,24u)),lumaRange=int(std::min(step,8u));
            int weight=range*4,coSum=center[1]*weight,cgSum=center[2]*weight,yWeight=64,ySum=center[0]*yWeight;
            for(int dy=-2;dy<=2;dy+=2)for(int dx=-2;dx<=2;dx+=2){if(!dx&&!dy)continue;
                const int spacing=adjacent?2:1;
                const auto n=color(uint32_t(std::clamp(int(x)+dx/spacing,0,int(source.width)-1)),uint32_t(std::clamp(int(y)+dy/spacing,0,int(source.height)-1)));
                const int luma=std::abs(center[0]-n[0])/4,difference=std::max({luma,std::abs(center[1]-n[1])/8,std::abs(center[2]-n[2])/16});
                const int w=range-std::min(range,difference);weight+=w;coSum+=n[1]*w;cgSum+=n[2]*w;
                const int wy=lumaRange-std::min(lumaRange,luma);yWeight+=wy;ySum+=n[0]*wy;
            }
            const int yy=divide(ySum,yWeight),co=divide(coSum,weight),cg=divide(cgSum,weight);const auto at=(size_t(y)*source.width+x)*4;
            expected.pixels[at]=uint8_t(std::clamp(divide(yy+2*co-cg,4),0,255));expected.pixels[at+1]=uint8_t(std::clamp(divide(yy+cg,4),0,255));expected.pixels[at+2]=uint8_t(std::clamp(divide(yy-2*co-cg,4),0,255));
        }
        auto actual=source;
        {ScopedComputeBackend gpu(ComputeBackend::Gpu);const auto before=gpuActivity();
            check(gpuReferenceFilter(actual,step,mask,adjacent),"Reference filter declined.");
            const auto after=gpuActivity();check(after.referenceFilters-before.referenceFilters==(step>=4?1u:0u),"Reference filter did not execute on GPU.");
            check(after.uploadedBytes-before.uploadedBytes==(step>=4?source.pixels.size()+mask.size()*4:0),"Reference filter uploaded float planes.");
            check(after.downloadedBytes-before.downloadedBytes==(step>=4?source.pixels.size():0),"Reference filter downloaded extra data.");}
        check(actual.pixels==expected.pixels,"GPU reference filter changed canonical integer pixels.");
        {ScopedComputeBackend cpu(ComputeBackend::Cpu);const auto before=actual.pixels;
            check(!gpuReferenceFilter(actual,step,mask,adjacent)&&actual.pixels==before,"Reference filter ignored CPU selection.");}
    }
    // Independent mode-selection oracle around the established integer warp
    // and residual transform. Exercise different source/decoded references,
    // exact skips, affine motion, clipping, partial 8x8/2x2 cells and padding.
    for(auto dimensions:{std::array{512u,512u},std::array{513u,517u},std::array{515u,518u}}){
        const auto width=dimensions[0],height=dimensions[1],w=(width+127)&~127u,h=(height+127)&~127u,cols=(width+7)/8,rows=(height+7)/8;
        Frame source{width,height,0,1,Bytes(pixelBytes(width,height),255)},decoded=source;
        for(size_t i=0;i<source.pixels.size();i+=4)for(unsigned c=0;c<3;++c){source.pixels[i+c]=uint8_t(rng());decoded.pixels[i+c]=uint8_t(std::clamp(int(source.pixels[i+c])+int(rng()%9)-4,0,255));}
        std::vector<uint32_t> vectors(size_t((width+31)/32)*((height+31)/32)),affine(vectors.size());
        for(size_t i=0;i<vectors.size();++i){vectors[i]=uint16_t(int16_t(int(rng()%31)-15))|(uint32_t(uint16_t(int16_t(int(rng()%31)-15)))<<16);affine[i]=rng();}
        for(const auto bias:{std::array<int,3>{0,0,0},std::array<int,3>{-32,1,32}})for(uint32_t tolerance:{0u,6u,16u}){
            ScopedComputeBackend compute(ComputeBackend::Gpu);
            auto predicted=decoded,witness=source;
            check(gpuMotionWarp(decoded,32,vectors,bias,{},predicted.pixels,true,affine)&&gpuMotionWarp(source,32,vectors,bias,{},witness.pixels,true,affine),"Predictive preparation oracle warp declined.");
            auto current=source;
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)for(unsigned c=0;c<3;++c){const auto at=(size_t(y)*width+x)*4+c;
                const auto mode=(x/8+3*(y/8))%4;
                current.pixels[at]=uint8_t(std::clamp((mode==0?int(source.pixels[at])+bias[c]:int(witness.pixels[at]))+(mode==2?1:mode==3?int(rng()%15)-7:0),0,255));}
            Bytes keep(size_t(cols)*rows),actualKeep(keep.size(),199);auto actualPrediction=current;
            for(uint32_t ty=0;ty<rows;++ty)for(uint32_t tx=0;tx<cols;++tx){uint64_t moving=0,stationary=0;unsigned stationaryMax=0,movingMax=0,count=0;
                for(uint32_t y=ty*8;y<std::min(height,ty*8+8);++y)for(uint32_t x=tx*8;x<std::min(width,tx*8+8);++x)for(unsigned c=0;c<3;++c){const auto at=(size_t(y)*width+x)*4+c;
                    const int a=current.pixels[at],m=a-int(predicted.pixels[at]),s=a-std::clamp(int(decoded.pixels[at])+bias[c],0,255);
                    moving+=m*m;stationary+=s*s;stationaryMax=std::max(stationaryMax,unsigned(std::abs(a-std::clamp(int(source.pixels[at])+bias[c],0,255))));movingMax=std::max(movingMax,unsigned(std::abs(a-int(witness.pixels[at]))));++count;}
                const bool still=stationary<=moving||(stationaryMax<=tolerance&&stationary<=moving+uint64_t(count)*tolerance*tolerance/4);
                keep[size_t(ty)*cols+tx]=uint8_t((still?2:0)|((still?stationaryMax:movingMax)==0?1:0));
                if(still)for(uint32_t y=ty*8;y<std::min(height,ty*8+8);++y)for(uint32_t x=tx*8;x<std::min(width,tx*8+8);++x)for(unsigned c=0;c<3;++c){const auto at=(size_t(y)*width+x)*4+c;
                    predicted.pixels[at]=uint8_t(std::clamp(int(decoded.pixels[at])+bias[c],0,255));witness.pixels[at]=uint8_t(std::clamp(int(source.pixels[at])+bias[c],0,255));}
            }
            std::array<std::vector<float>,3> expected,actual;std::array<GpuPlane,3> expectedPlanes,actualPlanes;uint64_t coefficientBytes=0;
            for(unsigned c=0;c<3;++c){expected[c].resize(size_t(c?w/2:w)*(c?h/2:h));actual[c]=expected[c];expectedPlanes[c]={expected[c],c?w/2:w,c?h/2:h};actualPlanes[c]={actual[c],c?w/2:w,c?h/2:h};coefficientBytes+=actual[c].size()*4;}
            check(gpuPredictiveWavelet(current,predicted,witness,keep,tolerance,expectedPlanes),"Preparation oracle residual declined.");const auto before=gpuActivity();
            check(gpuPreparePredictive(current,source,decoded,vectors,affine,bias,tolerance,tolerance,actualPrediction.pixels,actualKeep,actualPlanes),"Resident predictive preparation declined.");
            const auto after=gpuActivity();
            if(actual!=expected||actualKeep!=keep||actualPrediction.pixels!=predicted.pixels){
                size_t modes=0,pixels=0,coefficients=0;for(size_t i=0;i<keep.size();++i)if(keep[i]!=actualKeep[i]){if(modes<5)std::cerr<<"mode "<<i<<": "<<unsigned(keep[i])<<" -> "<<unsigned(actualKeep[i])<<'\n';++modes;}
                for(size_t i=0;i<predicted.pixels.size();++i)if(predicted.pixels[i]!=actualPrediction.pixels[i])++pixels;
                for(unsigned c=0;c<3;++c)for(size_t i=0;i<actual[c].size();++i)if(actual[c][i]!=expected[c][i])++coefficients;
                std::cerr<<"Preparation "<<width<<'x'<<height<<" tolerance="<<tolerance<<" bias="<<bias[0]<<" mismatched modes="<<modes<<" pixels="<<pixels<<" coefficients="<<coefficients<<'\n';
            }
            check(actual==expected&&actualKeep==keep&&actualPrediction.pixels==predicted.pixels,"Resident predictive preparation changed coefficients, modes or prediction.");
            check(after.uploadedBytes-before.uploadedBytes==source.pixels.size()*2+vectors.size()*8&&after.downloadedBytes-before.downloadedBytes==coefficientBytes+predicted.pixels.size()+keep.size()*4,"Resident preparation transferred intermediate warps.");
            const auto saved=actual;const auto savedPixels=actualPrediction.pixels,savedKeep=actualKeep;
            {ScopedComputeBackend cpu(ComputeBackend::Cpu);check(!gpuPreparePredictive(current,source,decoded,vectors,affine,bias,tolerance,tolerance,actualPrediction.pixels,actualKeep,actualPlanes)&&actual==saved&&actualPrediction.pixels==savedPixels&&actualKeep==savedKeep,"Declined preparation changed host outputs.");}
        }
    }
    // Independent CPU preparation + the established GPU wavelet is an exact
    // oracle for the fused residual kernel, including feathered skip corners,
    // partial chroma cells and replicated padding on odd image borders.
    for(auto dimensions:{std::array{512u,512u},std::array{513u,517u},std::array{515u,518u}})for(uint32_t limit:{0u,6u,32u})for(bool lumaOnly:{false,true}){
        const auto width=dimensions[0],height=dimensions[1],w=(width+127)&~127u,h=(height+127)&~127u,cols=(width+7)/8,rows=(height+7)/8;
        Frame current{width,height,0,1,Bytes(pixelBytes(width,height))},predicted=current,witness=current;
        for(auto* f:{&current,&predicted,&witness})for(size_t i=0;i<f->pixels.size();i+=4){for(unsigned c=0;c<3;++c)f->pixels[i+c]=uint8_t(rng());f->pixels[i+3]=255;}
        Bytes keep(size_t(cols)*rows);for(auto& mode:keep)mode=uint8_t(rng()%4);
        auto blend=[&](uint32_t x,uint32_t y){if(!(keep[size_t(y/8)*cols+x/8]&1))return 4u;unsigned result=0;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const int xx=int(x/8)+dx,yy=int(y/8)+dy;if(xx<0||yy<0||xx>=int(cols)||yy>=int(rows)||keep[size_t(yy)*cols+xx]&1)continue;
                const int distance=std::max({dx*8-int(x%8),int(x%8)-(dx*8+7),dy*8-int(y%8),int(y%8)-(dy*8+7),0});if(distance<4)result=std::max(result,unsigned(4-distance));}return result;};
        std::array<std::vector<float>,3> expected,result;
        for(unsigned c=0;c<3;++c)expected[c].resize(size_t(c?w/2:w)*(c?h/2:h));
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){const auto at=(size_t(y)*width+x)*4;float d[3];
            const int carry=(int(predicted.pixels[at])-int(witness.pixels[at])+2*(int(predicted.pixels[at+1])-int(witness.pixels[at+1]))+int(predicted.pixels[at+2])-int(witness.pixels[at+2]))/4;
            for(unsigned c=0;c<3;++c){const int inherited=lumaOnly?std::clamp(carry,-int(limit),int(limit))/2:std::clamp(int(predicted.pixels[at+c])-int(witness.pixels[at+c]),-int(limit),int(limit));
                d[c]=blend(x,y)?float(int(current.pixels[at+c])-int(predicted.pixels[at+c])+inherited):0;}
            expected[0][size_t(y)*w+x]=(d[0]+2*d[1]+d[2])*.25f;
            const auto cp=size_t(y/2)*(w/2)+x/2;const float area=float(std::min(2u,width-(x/2)*2)*std::min(2u,height-(y/2)*2));
            expected[1][cp]+=(d[0]-d[2])*(.5f/area);expected[2][cp]+=(2*d[1]-d[0]-d[2])*(.25f/area);
        }
        for(unsigned c=0;c<3;++c){const auto stride=c?w/2:w,ph=c?h/2:h,vw=c?(width+1)/2:width,vh=c?(height+1)/2:height;
            for(uint32_t y=0;y<ph;++y){auto* target=expected[c].data()+size_t(y)*stride;const auto* source=expected[c].data()+size_t(std::min(y,vh-1))*stride;if(y>=vh)std::copy_n(source,vw,target);std::fill(target+vw,target+stride,source[vw-1]);}}
        result=expected;std::array<GpuPlane,3> referencePlanes,resultPlanes;
        for(unsigned c=0;c<3;++c){referencePlanes[c]={expected[c],c?w/2:w,c?h/2:h};resultPlanes[c]={result[c],c?w/2:w,c?h/2:h};}
        {ScopedComputeBackend compute(ComputeBackend::Gpu);check(gpuWavelet(referencePlanes,false),"Predictive oracle transform declined.");
            const auto before=gpuActivity();check(gpuPredictiveWavelet(current,predicted,witness,keep,limit,resultPlanes,lumaOnly),"Fused predictive wavelet declined.");
            check(gpuActivity().uploadedBytes-before.uploadedBytes==3*current.pixels.size()+keep.size()*4,"Fused residual uploaded float planes.");}
        check(result==expected,"Fused residual changed coefficients.");
        const auto saved=result;check(!gpuPredictiveWavelet(current,predicted,witness,keep,limit,resultPlanes,lumaOnly)&&result==saved,"CPU selection changed fused output.");
    }
    // Independent scalar oracle, including negative interpolation ties,
    // fractional border clamps, partial cells and stationary block modes.
    std::mt19937 motionRng(510);
    for(const auto size:{std::pair{512u,512u},std::pair{513u,517u}})for(uint32_t step:{0u,1u,4u,36u,256u})for(bool skip:{false,true})for(bool coefficients:{false,true}){
        const auto [width,height]=size;const auto w=(width+127)&~127u,h=(height+127)&~127u,cols=(width+31)/32,maskCols=(width+7)/8,maskRows=(height+7)/8;
        Frame source{width,height,0,1,Bytes(pixelBytes(width,height))};for(auto& v:source.pixels)v=uint8_t(motionRng());
        std::vector<uint32_t> vectors(size_t(cols)*((height+31)/32)),gradients(vectors.size());
        for(auto& v:vectors)v=uint16_t(int(motionRng()%1025)-512)|(uint32_t(uint16_t(int(motionRng()%1025)-512))<<16);for(auto& g:gradients)g=motionRng();
        Bytes modes(size_t(maskCols)*maskRows);for(auto& v:modes)v=skip?3:uint8_t(motionRng()%4);
        const std::array<int,3> bias{-32,7,32};std::array<std::vector<float>,3> samples;std::array<GpuPlane,3> planes;
        for(unsigned c=0;c<3;++c){samples[c].resize(size_t(c?w/2:w)*(c?h/2:h));for(auto& v:samples[c])v=float(int(motionRng()%2049)-1024)/64;planes[c]={samples[c],c?w/2:w,c?h/2:h};}
        if(step==0){const float half[]={.5f,std::nextafter(.5f,0.f),-.5f,std::nextafter(-.5f,0.f),1.5f,-1.5f,255.f,-255.f};
            for(size_t i=0;i<samples[0].size();++i)samples[0][i]=half[i%8];std::fill(samples[1].begin(),samples[1].end(),0);std::fill(samples[2].begin(),samples[2].end(),0);}
        samples[0][0]=.5f;
        if(coefficients&&step==256){
            for(auto& p:samples)std::fill(p.begin(),p.end(),0.f);
            if(!skip)for(unsigned c=0;c<3;++c){samples[c][0]=-.0f;for(size_t i=1;i<samples[c].size();i+=997)samples[c][i]=float(int(i%129)-64)/16;}
        }
        auto inverseSamples=samples;
        if(coefficients)for(unsigned c=0;c<3;++c)waveletTransform(inverseSamples[c],c?w/2:w,c?h/2:h,3,true);
        Frame expected=source;
        {ScopedComputeBackend gpu(ComputeBackend::Gpu);check(gpuMotionWarp(source,32,vectors,bias,modes,expected.pixels,true,gradients),"Resident color oracle prediction failed.");}
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){unsigned blend=4;
            if(modes[size_t(y/8)*maskCols+x/8]&1){blend=0;
                for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const int xx=int(x/8)+dx,yy=int(y/8)+dy;
                    if(xx<0||yy<0||xx>=int(maskCols)||yy>=int(maskRows)||(modes[size_t(yy)*maskCols+xx]&1))continue;
                    const int lx=int(x%8),ly=int(y%8),distance=std::max({0,dx*8-lx,lx-(dx*8+7),dy*8-ly,ly-(dy*8+7)});if(distance<4)blend=std::max(blend,unsigned(4-distance));}}
            if(!blend)continue;
            const float sx=std::max(0.f,(x+.5f)/2-.5f),sy=std::max(0.f,(y+.5f)/2-.5f);const auto ax=uint32_t(sx),ay=uint32_t(sy),bx=std::min(ax+1,w/2-1),by=std::min(ay+1,h/2-1);const float fx=sx-ax,fy=sy-ay;
            auto chroma=[&](unsigned c){auto at=[&](uint32_t xx,uint32_t yy){return inverseSamples[c][size_t(yy)*(w/2)+xx];};return (at(ax,ay)*(1-fx)+at(bx,ay)*fx)*(1-fy)+(at(ax,by)*(1-fx)+at(bx,by)*fx)*fy;};
            const float yy=inverseSamples[0][size_t(y)*w+x],co=chroma(1),cg=chroma(2),delta[]={yy+co-cg,yy+cg,yy-co-cg};const auto at=(size_t(y)*width+x)*4;
            for(unsigned c=0;c<3;++c){const float correction=std::clamp(delta[c]*blend*.25f,-255.f,255.f);const int rounded=int(double(correction)+(correction<0?-.5:.5));expected.pixels[at+c]=uint8_t(std::clamp(int(expected.pixels[at+c])+rounded,0,255));}
        }
        {ScopedComputeBackend gpu(ComputeBackend::Gpu);if(step>=4)check(gpuReferenceFilter(expected,step,modes),"Resident filter oracle declined.");}
        Bytes guarded(source.pixels.size()+32,197);auto target=std::span(guarded).subspan(16,source.pixels.size());unsigned calls=0;if(!coefficients)samples[0][0]=0;const auto uploadedSamples=samples;
        {ScopedComputeBackend gpu(ComputeBackend::Gpu);const auto before=gpuActivity();
            check(gpuReconstructMotion(source,vectors,bias,modes,gradients,planes,step,target,[&]{++calls;if(coefficients)for(unsigned c=0;c<3;++c)waveletTransform(samples[c],c?w/2:w,c?h/2:h,3,true);else samples[0][0]=.5f;},coefficients),"Resident motion reconstruction declined.");const auto after=gpuActivity();
            const bool deviceInverse=coefficients&&after.transforms>before.transforms;
            const bool sparse=after.sparseCoefficientUploads>before.sparseCoefficientUploads;
            size_t coefficientBytes=(samples[0].size()+samples[1].size()+samples[2].size())*4;
            if(sparse){coefficientBytes=0;for(const auto& p:uploadedSamples)for(float v:p)coefficientBytes+=std::bit_cast<uint32_t>(v)!=0?8:0;}
            check(!deviceInverse||step!=256||sparse,"Sparse/all-zero coefficients were uploaded densely.");
            const uint64_t upload=source.pixels.size()+vectors.size()*4+gradients.size()*4+modes.size()*4+coefficientBytes;
            check(calls==(deviceInverse?0u:1u)&&(!deviceInverse||samples==uploadedSamples)&&after.motionReconstructions-before.motionReconstructions==1&&after.uploadedBytes-before.uploadedBytes==upload&&after.downloadedBytes-before.downloadedBytes==source.pixels.size(),"Resident reconstruction repeated work, modified coefficients or transferred intermediate RGBA.");
        }
        check(std::equal(expected.pixels.begin(),expected.pixels.end(),target.begin()),"Resident residual color/filter differs from canonical CPU.");
        check(std::all_of(guarded.begin(),guarded.begin()+16,[](auto v){return v==197;})&&std::all_of(guarded.end()-16,guarded.end(),[](auto v){return v==197;}),"Resident output guard changed.");
        const auto saved=guarded;calls=0;check(!gpuReconstructMotion(source,vectors,bias,modes,gradients,planes,step,target,[&]{++calls;})&&!calls&&guarded==saved,"CPU selection modified resident output/called callback.");
        if(!skip&&step==0&&!coefficients){ScopedComputeBackend gpu(ComputeBackend::Gpu);const auto before=gpuActivity();bool propagated=false;
            try{gpuReconstructMotion(source,vectors,bias,modes,gradients,planes,step,target,[]{throw std::logic_error("Resident CPU marker");});}
            catch(const std::logic_error& e){propagated=std::string_view(e.what())=="Resident CPU marker";}
            check(propagated&&guarded==saved&&gpuActivity().fallbacks==before.fallbacks,"Resident CPU exception was retried or changed output.");}
    }
    for(uint32_t cell:{16u,32u})for(bool masked:{false,true})for(bool independent:{false,true}){
        constexpr uint32_t w=513,h=517;Frame source{w,h,0,1,Bytes(pixelBytes(w,h))};for(auto& v:source.pixels)v=uint8_t(motionRng());const auto original=source.pixels;
        const uint32_t cols=(w+cell-1)/cell,rows=(h+cell-1)/cell,maskCols=(w+7)/8;
        std::vector<uint32_t> vectors(size_t(cols)*rows);for(auto& v:vectors){const auto x=int(motionRng()%1025)-512,y=int(motionRng()%1025)-512;v=uint16_t(int16_t(x))|(uint32_t(uint16_t(int16_t(y)))<<16);}
        std::vector<uint32_t> gradients(independent&&cell==32?vectors.size():0);for(auto& v:gradients)v=motionRng();
        Bytes modes(masked?size_t(maskCols)*((h+7)/8):0);for(auto& v:modes)v=uint8_t(motionRng()%4);
        for(const auto bias:{std::array<int,3>{-32,0,32},std::array<int,3>{1,-1,0}}){
            Bytes reference(source.pixels.size()),result(reference.size(),71);
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const int gx=int(x)-int(cell/2),gy=int(y)-int(cell/2),lx=gx<0?0:std::min(gx/int(cell),int(cols)-1),ly=gy<0?0:std::min(gy/int(cell),int(rows)-1),hx=std::min(lx+1,int(cols)-1),hy=std::min(ly+1,int(rows)-1),fx=gx<0?0:gx%int(cell),fy=gy<0?0:gy%int(cell);
                int motion[2]{};for(int axis=0;axis<2;++axis){auto value=[&](int xx,int yy){const uint32_t v=vectors[size_t(yy)*cols+xx];const uint32_t bits=(v>>(16*axis))&65535;return bits>=32768?int(bits)-65536:int(bits);};
                    const int sum=(value(lx,ly)*(int(cell)-fx)+value(hx,ly)*fx)*(int(cell)-fy)+(value(lx,hy)*(int(cell)-fx)+value(hx,hy)*fx)*fy,denominator=int(cell*cell);motion[axis]=sum>=0?(sum+denominator/2)/denominator:-((-sum+denominator/2)/denominator);}
                if(independent){const auto v=vectors[size_t(y/cell)*cols+x/cell];motion[0]=int(int16_t(uint16_t(v)));motion[1]=int(int16_t(uint16_t(v>>16)));}
                if(!gradients.empty()){const auto g=gradients[size_t(y/32)*cols+x/32];int a[4];for(int n=0;n<4;++n)a[n]=int(int8_t(uint8_t(g>>(n*8))));const int lx=int(x%32)-16,ly=int(y%32)-16;auto round=[](int v){return v>=0?(v+128)/256:-((-v+128)/256);};motion[0]+=round(a[0]*lx+a[1]*ly);motion[1]+=round(a[2]*lx+a[3]*ly);}
                if(masked&&(modes[size_t(y/8)*maskCols+x/8]&2))motion[0]=motion[1]=0;
                const int sx=std::clamp(int(x)*4+motion[0],0,int(w-1)*4),sy=std::clamp(int(y)*4+motion[1],0,int(h-1)*4),ax=sx/4,ay=sy/4,bx=std::min(ax+1,int(w)-1),by=std::min(ay+1,int(h)-1),tx=sx%4,ty=sy%4;
                const auto at=(size_t(y)*w+x)*4;for(int c=0;c<3;++c){auto sample=[&](int xx,int yy){return int(source.pixels[(size_t(yy)*w+xx)*4+c]);};const int value=((sample(ax,ay)*(4-tx)+sample(bx,ay)*tx)*(4-ty)+(sample(ax,by)*(4-tx)+sample(bx,by)*tx)*ty+8)/16;reference[at+c]=uint8_t(std::clamp(value+bias[c],0,255));}reference[at+3]=255;
            }
            {ScopedComputeBackend gpu(ComputeBackend::Gpu);const auto before=gpuActivity();unsigned calls=0;
                check(gpuMotionWarp(source,cell,vectors,bias,modes,result,independent,gradients,[&]{++calls;}),"GPU motion prediction declined.");check(calls==1,"Independent CPU work was skipped or repeated.");const auto after=gpuActivity();
                check(after.uploadedBytes-before.uploadedBytes==source.pixels.size()+vectors.size()*4+modes.size()*4+gradients.size()*4&&after.downloadedBytes-before.downloadedBytes==result.size(),"GPU motion transferred unexpected buffers.");}
            check(result==reference&&source.pixels==original,"GPU motion prediction differs from integer oracle.");
        }
        Bytes untouched(source.pixels.size(),41);const auto saved=untouched;check(!gpuMotionWarp(source,cell,vectors,{0,0,0},modes,untouched)&&untouched==saved,"CPU selection ran or modified GPU motion output.");
        unsigned declinedCalls=0;check(!gpuMotionWarp(source,cell,vectors,{0,0,0},modes,untouched,independent,gradients,[&]{++declinedCalls;})&&!declinedCalls&&untouched==saved,"Declined GPU work called its CPU callback.");
        {ScopedComputeBackend gpu(ComputeBackend::Gpu);const auto before=gpuActivity();unsigned calls=0;bool propagated=false;
            try{gpuMotionWarp(source,cell,vectors,{0,0,0},modes,untouched,independent,gradients,[&]{++calls;throw std::logic_error("CPU overlap marker");});}
            catch(const std::logic_error& e){propagated=std::string_view(e.what())=="CPU overlap marker";}
            check(propagated&&calls==1&&gpuActivity().fallbacks==before.fallbacks,"CPU failure was retried/classified as a GPU error.");
            Bytes recovered(source.pixels.size());check(gpuMotionWarp(source,cell,vectors,{0,0,0},modes,recovered,independent,gradients),"GPU lock/resources did not recover after CPU failure.");
        }
        untouched=saved;
        bool rejected=false;try{gpuMotionWarp(source,cell,vectors,{33,0,0},modes,untouched);}catch(const std::exception&){rejected=true;}check(rejected&&untouched==saved,"Invalid motion bias modified output.");
    }
    {
        constexpr uint32_t w=513,h=517,cols=(w+31)/32,rows=(h+31)/32;
        Frame old{w,h,0,1,Bytes(pixelBytes(w,h))},current=old;
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=(size_t(y)*w+x)*4;
            const auto v=uint8_t(100+40*std::sin(x*.113+y*.017)+30*std::cos(y*.127-x*.023)+15*std::sin(x*.271-y*.191));
            old.pixels[at]=old.pixels[at+1]=old.pixels[at+2]=v;old.pixels[at+3]=255;}
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const int dx=x<w/2?-6:5,dy=x<w/2?3:-4;const auto from=(size_t(std::clamp(int(y)+dy,0,int(h)-1))*w+std::clamp(int(x)+dx,0,int(w)-1))*4;
            std::copy_n(old.pixels.data()+from,4,current.pixels.data()+(size_t(y)*w+x)*4);}
        std::vector<uint32_t> vectors(size_t(cols)*rows,0xabababab);
        {ScopedComputeBackend gpu(ComputeBackend::Gpu);const auto before=gpuActivity();check(gpuBlockMotion(current,old,vectors),"GPU hierarchical motion declined.");const auto after=gpuActivity();
            check(after.uploadedBytes-before.uploadedBytes==old.pixels.size()*2&&after.downloadedBytes-before.downloadedBytes==vectors.size()*4,"GPU motion search downloaded full frames or uploaded intermediate seeds.");}
        Bytes predicted(current.pixels.size());{ScopedComputeBackend gpu(ComputeBackend::Gpu);check(gpuMotionWarp(old,32,vectors,{0,0,0},{},predicted,true),"Independent GPU motion warp declined.");}
        uint64_t before=0,after=0;for(uint32_t y=64;y<h-64;++y)for(uint32_t x=64;x<w-64;++x){if(std::abs(int(x)-int(w/2))<40)continue;const auto at=(size_t(y)*w+x)*4;const int a=int(current.pixels[at])-old.pixels[at],b=int(current.pixels[at])-predicted[at];before+=a*a;after+=b*b;}
        std::cout<<"GPU independent object motion SSE "<<before<<" -> "<<after<<'\n';check(after*10<before,"GPU block motion failed to follow independently moving regions.");
        const auto saved=vectors;check(!gpuBlockMotion(current,old,vectors)&&vectors==saved,"CPU selection modified GPU motion vectors.");
        // Reuse the same allocations with different content: pointer equality
        // alone must never make a mutable reference appear resident/unchanged.
        std::fill(old.pixels.begin(),old.pixels.end(),91);std::fill(current.pixels.begin(),current.pixels.end(),94);
        {ScopedComputeBackend gpu(ComputeBackend::Gpu);check(gpuBlockMotion(current,old,vectors),"Flat GPU motion declined.");}
        check(std::all_of(vectors.begin(),vectors.end(),[](uint32_t v){return v==0;}),"Uniform brightness change generated false block motion.");
        bool rejected=false;try{ScopedComputeBackend gpu(ComputeBackend::Gpu);gpuBlockMotion(current,old,std::span(vectors).first(vectors.size()-1));}catch(const std::exception&){rejected=true;}
        check(rejected,"GPU motion search accepted a truncated vector array.");
    }
    for(auto [w,h]:{std::pair{64,64},std::pair{128,128},std::pair{192,320},std::pair{256,256},std::pair{320,448},std::pair{1024,768},std::pair{2176,1280}}){
        for(bool inverse:{false,true})for(int first:{0,1}){
            std::vector<float> input(size_t(w)*h);
            for(auto& v:input)v=float(int(rng()%100001)-50000)*.002f;
            auto cpu=input,gpu=input;
            waveletTransform(cpu,w,h,3,inverse,true,true,first,6-first);
            {
                ScopedComputeBackend scope(ComputeBackend::Gpu);
                GpuPlane plane{gpu,uint32_t(w),uint32_t(h)};
                if(uint64_t(w>>first)*(h>>first)>=256*256)check(gpuWavelet({&plane,1},inverse,first,6-first),"GPU transform unexpectedly declined.");
                else waveletTransform(gpu,w,h,3,inverse,true,true,first,6-first);
            }
            double sum=0,error=0;
            for(size_t i=0;i<cpu.size();++i){const double e=std::abs(cpu[i]-gpu[i]);error=std::max(error,e);sum+=e;}
            worst=std::max(worst,error);
            std::cout<<w<<'x'<<h<<" inverse="<<inverse<<" first="<<first<<" max="<<error<<" mean="<<sum/cpu.size()<<'\n';
            check(error<.004&&sum/cpu.size()<.0001,"GPU CDF 9/7 differs from production CPU.");
            if(inverse){auto exact=input;GpuPlane p{exact,uint32_t(w),uint32_t(h)};
                ScopedComputeBackend compute(ComputeBackend::Gpu);
                const auto beforeCanonical=gpuActivity();if(gpuCanonicalInverse({&p,1},first,6-first)){
                    if(exact!=cpu){size_t changed=0;for(size_t i=0;i<cpu.size();++i)if(exact[i]!=cpu[i]){if(!changed)std::cerr<<"Canonical inverse mismatch "<<w<<'x'<<h<<" first="<<first<<" index="<<i<<" CPU="<<cpu[i]<<" GPU="<<exact[i]<<'\n';++changed;}std::cerr<<"Canonical changed samples: "<<changed<<'\n';}
                    check(exact==cpu,"Canonical GPU inverse differs from exact CPU floats.");
                }else{check(gpuActivity().fallbacks==beforeCanonical.fallbacks,"Canonical GPU inverse failed rather than declining support.");std::cout<<"SKIP optional canonical GPU inverse\n";}
            }
        }
    }
    for(auto [w,h]:{std::pair{512u,515u},std::pair{1024u,768u}}){
        Frame source{w,h,17,23,Bytes(pixelBytes(w,h),255)};
        for(size_t i=0;i<source.pixels.size();i+=4)for(int c=0;c<3;++c)source.pixels[i+c]=uint8_t(rng());
        const uint32_t ow=w*3/4,oh=h*3/4;
        auto cpu=resamplePhoto(source,ow,oh,3,{},2);
        Frame gpu;
        {ScopedComputeBackend scope(ComputeBackend::Gpu);gpu=resamplePhoto(source,ow,oh,3,{},2);}
        double error=0;size_t changed=0;
        for(size_t i=0;i<cpu.pixels.size();++i){const int d=std::abs(int(cpu.pixels[i])-gpu.pixels[i]);check(d<=1,"GPU resample changed rounding by more than 1.");error+=d;changed+=d!=0;}
        check(error/cpu.pixels.size()<.002,"GPU resample is not sufficiently close.");
        std::cout<<"Resample changed bytes "<<changed<<" / "<<cpu.pixels.size()<<'\n';
        auto codec=makeCodec(CodecId::Fajk);EncodeOptions o;o.threads=3;o.targetBytes=6000;
        SourcePriority cpuPriority(source,3,31);
        std::unique_ptr<SourcePriority> gpuPriority;
        {ScopedComputeBackend scope(ComputeBackend::Gpu);gpuPriority=std::make_unique<SourcePriority>(source,3,31);}
        for(uint32_t y=0;y<h;y+=13)for(uint32_t x=0;x<w;x+=17)for(uint32_t size:{8u,32u,128u}){
            check(cpuPriority.bandRate(x,y,size,size)==gpuPriority->bandRate(x,y,size,size),"GPU analysis changed source classification.");
            check(cpuPriority.flatCoverage(x,y,size,size,100)==gpuPriority->flatCoverage(x,y,size,size,100),"GPU flat coverage changed.");
            check(cpuPriority.hasDirectEdge(x,y,size,size)==gpuPriority->hasDirectEdge(x,y,size,size),"GPU analysis changed source edges.");
        }
        auto packet=codec->encode(source,o);auto baseline=codec->decode(packet,w,h);
        codec->setComputeBackend(ComputeBackend::Gpu);
        auto decoded=codec->decode(packet,w,h);
        double total=0;int maximum=0;
        for(size_t i=0;i<decoded.pixels.size();++i){const int d=std::abs(int(decoded.pixels[i])-baseline.pixels[i]);total+=d;maximum=std::max(maximum,d);}
        std::cout<<"Decode max="<<maximum<<" mean="<<total/decoded.pixels.size()<<'\n';
        check(maximum<=3&&total/decoded.pixels.size()<.02,"GPU decode quality regression.");
        o.backend=ComputeBackend::Gpu;auto coded=codec->encode(source,o);
        check(coded.data.size()<=o.targetBytes,"GPU encode exceeded budget.");
        o.budgetHeatmap=std::make_shared<BudgetHeatmap>();
        check(codec->encode(source,o).data==coded.data,"GPU debug capture changed the packet.");
        o.budgetHeatmap.reset();
        codec->setComputeBackend(ComputeBackend::Cpu);codec->decode(coded,w,h).validate();
        for(uint32_t sampling:{420u,422u,444u})for(uint32_t budget:{6000u,40000u}){
            EncodeOptions settings;settings.threads=3;settings.targetBytes=budget;settings.chroma420=sampling==420;settings.chroma422=sampling==422;
            const auto sample=codec->encode(source,settings);
            codec->setComputeBackend(ComputeBackend::Cpu);const auto a=codec->decode(sample,w,h);
            codec->setComputeBackend(ComputeBackend::Gpu);const auto b=codec->decode(sample,w,h);
            double sum=0;int maximum=0;
            for(size_t i=0;i<a.pixels.size();++i){const int d=std::abs(int(a.pixels[i])-b.pixels[i]);sum+=d;maximum=std::max(maximum,d);}
            check(maximum<=4&&sum/a.pixels.size()<.02,"GPU native filter/chroma quality regression.");
        }
        // Interleave different-sized jobs, then reuse the original tap cache.
        // Cached resampler geometry must not reuse resized analysis/decode buffers.
        Frame again;
        {ScopedComputeBackend scope(ComputeBackend::Gpu);again=resamplePhoto(source,ow,oh,3,{},2);}
        check(again.pixels==gpu.pixels,"GPU buffer cache was corrupted by another operation.");
    }
    // Compare the new recursive postfilter against its CPU arithmetic directly,
    // including partial tiles, transparent pixels, skip masks and both FMA modes.
    for(auto [w,h]:{std::pair{513u,515u},std::pair{768u,512u}}){
        Bytes source(pixelBytes(w,h));
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=(size_t(y)*w+x)*4;
            for(uint32_t c=0;c<3;++c)source[at+c]=uint8_t(std::clamp(int(128+100*std::sin((x+c*7)*.07+std::cos(y*.02)*4)+20*std::sin(y*.1))+int(rng()%7)-3,0,255));
            source[at+3]=(x%37==0&&y%19==0)?128:255;
        }
        Bytes skip(size_t((w+95)/96)*((h+95)/96));for(size_t i=0;i<skip.size();++i)skip[i]=i%5==0;
        for(bool fused:{false,true})for(float amount:{.1f,.8f}){
            const float stronger=amount>.2f?.3f:0,curvature=amount>.2f?.27f:0;
            const auto mask=amount>.2f?std::span<const uint8_t>(skip):std::span<const uint8_t>{};
            auto cpu=source,gpu=source;
            recursiveContourCleanup(cpu,w,h,3,amount,stronger,source,curvature,mask,fused);
            {ScopedComputeBackend scope(ComputeBackend::Gpu);check(gpuContourCleanup(gpu,source,w,h,amount,stronger,curvature,mask,fused),"GPU contour filter declined.");}
            int maximum=0;double sum=0;size_t changed=0;
            for(size_t i=0;i<cpu.size();++i){const int d=std::abs(int(cpu[i])-gpu[i]);maximum=std::max(maximum,d);sum+=d;changed+=d!=0;}
            std::cout<<"Contour "<<w<<'x'<<h<<" fused="<<fused<<" amount="<<amount<<" max="<<maximum<<" mean="<<sum/cpu.size()<<" changed="<<changed<<'\n';
            check(maximum<=1&&sum/cpu.size()<.002,"GPU recursive contour quality regression.");
            if(fused&&amount>.2f){
                auto integrated=source;const auto before=gpuActivity().contourFilters;
                {ScopedComputeBackend scope(ComputeBackend::Gpu);recursiveContourCleanup(integrated,w,h,1,amount,stronger,{},curvature,mask,fused);}
                check(integrated==gpu&&gpuActivity().contourFilters==before+1,"Recursive filter did not select GPU for a small CPU budget.");
                integrated=source;
                {ScopedComputeBackend scope(ComputeBackend::Gpu);recursiveContourCleanup(integrated,w,h,3,amount,stronger,source,curvature,mask,fused);}
                check(integrated==cpu&&gpuActivity().contourFilters==before+1,"Recursive filter added a GPU transfer with a large CPU budget.");
            }
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const size_t at=(size_t(y)*w+x)*4;
                check(gpu[at+3]==source[at+3],"GPU contour changed alpha.");
                if(amount>.2f&&(source[at+3]!=255||skip[size_t(y/96)*((w+95)/96)+x/96]))
                    for(uint32_t c=0;c<4;++c)check(gpu[at+c]==source[at+c],"GPU contour changed a protected pixel.");
            }
        }
    }
    // The color model uses the very same GPU-converted planes. Compare its
    // double reductions and final quantized patch equations
    // against the original CPU algorithm, including clamped partial cells.
    for(auto [w,h]:{std::pair{513u,517u},std::pair{768u,577u}}){
        Frame frame{w,h,0,1,Bytes(pixelBytes(w,h),255)};
        for(size_t at=0;at<frame.pixels.size();at+=4)for(int c=0;c<3;++c)frame.pixels[at+c]=uint8_t(rng());
        const uint32_t stride=(w+63)&~63u,ph=(h+63)&~63u;
        WaveletSourceColor source;source.width=w;source.height=h;
        for(auto& p:source.planes)p.resize(size_t(stride)*ph);
        std::array<GpuPlane,3> planes{{{source.planes[0],stride,ph},{source.planes[1],stride,ph},{source.planes[2],stride,ph}}};
        for(uint32_t tile:{32u,64u}){
            const uint32_t cols=(w+tile-1)/tile,rows=(h+tile-1)/tile;
            std::vector<GpuColorStatistics> summaries(size_t(cols)*rows);
            source.fittedPatches.resize(tile==64?size_t(cols+1)*(rows+1):0);
            bool supported=false;
            {ScopedComputeBackend scope(ComputeBackend::Gpu);supported=gpuSourceColorModel(frame,planes,tile,summaries,source.fittedPatches);}
            if(!supported){source.fittedPatches.clear();std::cout<<"SKIP optional double color model\n";continue;}
            for(uint32_t ty=0;ty<rows;++ty)for(uint32_t tx=0;tx<cols;++tx){
                GpuColorStatistics reference;
                for(uint32_t y=ty*tile;y<std::min(h,(ty+1)*tile);++y)for(uint32_t x=tx*tile;x<std::min(w,(tx+1)*tile);++x){
                    const size_t at=size_t(y)*stride+x;const double l=source.planes[0][at];reference.sum+=l;reference.square+=l*l;
                    for(int c=0;c<2;++c){const double color=source.planes[c+1][at];reference.color[c]+=color;reference.cross[c]+=l*color;}++reference.count;
                }
                const auto& s=summaries[size_t(ty)*cols+tx];
                auto close=[](double a,double b){return std::abs(a-b)<=1e-11*std::max(1.,std::abs(a));};
                check(close(reference.sum,s.sum)&&close(reference.square,s.square)&&reference.count==s.count,"GPU color reduction lost precision.");
                for(int c=0;c<2;++c)check(close(reference.color[c],s.color[c])&&close(reference.cross[c],s.cross[c]),"GPU color covariance lost precision.");
            }
            if(tile==64){
                Bytes a(size_t(cols)*rows*2);for(auto& v:a)v=uint8_t(int8_t(int(rng()%49)-24));auto b=a;
                refineInterpolatedChroma(source,tile,3,a);
                source.fittedPatches.clear();refineInterpolatedChroma(source,tile,3,b);
                check(a==b,"GPU chroma patch statistics changed quantized Jacobi correction.");
            }
        }
    }
    // A downsampled image already resides on the GPU. Reusing that exact
    // immutable output must not upload it again for subsequent analysis.
    {
        Frame frame{1024,768,0,1,Bytes(pixelBytes(1024,768),255)};
        for(size_t at=0;at<frame.pixels.size();at+=4)frame.pixels[at]=uint8_t(rng());
        ScopedComputeBackend scope(ComputeBackend::Gpu);
        auto small=resamplePhoto(frame,768,576,3,{},2);const auto bytes=gpuActivity().uploadedBytes;
        std::vector<GpuSourceStatistics> values(size_t(48)*36);
        check(gpuSourceStatistics(small,16,0,values),"Resampled source reuse test declined.");
        check(gpuActivity().uploadedBytes==bytes,"GPU resampler uploaded its own result again.");
        auto again=resamplePhoto(frame,768,576,3,{},2);
        check(again.pixels==small.pixels,"Retained resampler buffer lost its writable view.");
    }
    // Finite edge interpolation, border clamping and the AVX2 eight-lane
    // activation must survive odd dimensions and partially filled groups.
    for(auto [iw,ih,ow,oh]:{std::array{257u,259u,513u,517u},std::array{512u,512u,768u,777u}}){
        Frame source{iw,ih,0,1,Bytes(pixelBytes(iw,ih),255)};
        std::vector<int> xl(ow),yl(oh);std::vector<float> xf(ow),yf(oh);
        for(uint32_t x=0;x<ow;++x){double p=(x+.5)*iw/ow-.5;xl[x]=int(std::floor(p));xf[x]=float(p-xl[x]);}
        for(uint32_t y=0;y<oh;++y){double p=(y+.5)*ih/oh-.5;yl[y]=int(std::floor(p));yf[y]=float(p-yl[y]);}
        for(bool smooth:{false,true})for(float sharpness:{0.f,.7f,1.5f}){
            for(uint32_t y=0;y<ih;++y)for(uint32_t x=0;x<iw;++x)for(uint32_t c=0;c<3;++c)
                source.pixels[(size_t(y)*iw+x)*4+c]=smooth?uint8_t(((x/19+y/23+c)*3+(x%17==0?80:0))%256):uint8_t(rng());
            Bytes cpu(pixelBytes(ow,oh)),gpu(cpu.size());
            for(uint32_t y=0;y<oh;++y)fastEdgeUpscaleRow(source.pixels.data(),iw,ih,cpu.data()+size_t(y)*ow*4,ow,xl.data(),xf.data(),yl[y],yf[y],sharpness);
            {ScopedComputeBackend scope(ComputeBackend::Gpu);check(gpuEdgeUpscale(source,ow,oh,gpu,xl,xf,yl,yf,sharpness),"GPU edge upscale declined.");}
            int maximum=0;double sum=0;
            for(size_t i=0;i<cpu.size();++i){int d=std::abs(int(cpu[i])-gpu[i]);maximum=std::max(maximum,d);sum+=d;}
            std::cout<<"Edge upscale "<<ow<<'x'<<oh<<" sharpness="<<sharpness<<" max="<<maximum<<" mean="<<sum/cpu.size()<<'\n';
            check(maximum<=1&&sum/cpu.size()<.00001,"GPU edge interpolation changed reconstruction.");
        }
    }
    // The joined contour/upscale chain must equal two separate GPU operations,
    // while transferring only one source and one final output (plus axes).
    {
        constexpr uint32_t iw=513,ih=515,ow=769,oh=777;
        Frame source{iw,ih,0,1,Bytes(pixelBytes(iw,ih),255)};
        for(size_t at=0;at<source.pixels.size();at+=4)for(int c=0;c<3;++c)source.pixels[at+c]=uint8_t(rng());
        const auto saved=source.pixels;
        std::vector<int> xl(ow),yl(oh);std::vector<float> xf(ow),yf(oh);
        for(uint32_t x=0;x<ow;++x){double p=(x+.5)*iw/ow-.5;xl[x]=int(std::floor(p));xf[x]=float(p-xl[x]);}
        for(uint32_t y=0;y<oh;++y){double p=(y+.5)*ih/oh-.5;yl[y]=int(std::floor(p));yf[y]=float(p-yl[y]);}
        for(bool fused:{false,true})for(float amount:{.1f,.8f}){
            ScopedComputeBackend scope(ComputeBackend::Gpu);
            const float stronger=amount>.2f?.3f:0,curvature=amount>.2f?.27f:0;
            auto intermediate=source;Bytes separate(pixelBytes(ow,oh)),joined(separate.size());
            check(gpuContourCleanup(intermediate.pixels,source.pixels,iw,ih,amount,stronger,curvature,{},fused),"Separate contour declined.");
            check(gpuEdgeUpscale(intermediate,ow,oh,separate,xl,xf,yl,yf,.8f),"Separate upscale declined.");
            const auto before=gpuActivity();
            check(gpuContourUpscale(source,ow,oh,joined,xl,xf,yl,yf,.8f,amount,stronger,curvature,{},fused),"Joined contour upscale declined.");
            const auto after=gpuActivity();
            check(joined==separate&&source.pixels==saved,"Joined GPU chain changed pixels or source.");
            check(after.uploadedBytes-before.uploadedBytes==source.pixels.size()+size_t(ow+oh)*8&&
                  after.downloadedBytes-before.downloadedBytes==joined.size(),"Joined GPU chain transferred its intermediate.");
        }
    }
    // Stable video geometry alternates native and displayed dimensions every
    // frame. Neither output/readback pair may be reallocated after warming.
    {
        constexpr uint32_t w=512,h=512,ow=768,oh=777;
        std::array<std::vector<float>,3> samples{std::vector<float>(w*h),std::vector<float>(w*h/4),std::vector<float>(w*h/4)};
        for(auto& p:samples)for(size_t i=0;i<p.size();i+=97)p[i]=float(int(i%17)-8);
        std::array<GpuPlane,3> planes{{{samples[0],w,h},{samples[1],w/2,h/2},{samples[2],w/2,h/2}}};
        const auto saved=samples;Frame native{w,h,0,1,Bytes(pixelBytes(w,h))};Bytes displayed(pixelBytes(ow,oh)),expected;
        std::vector<int> xl(ow),yl(oh);std::vector<float> xf(ow),yf(oh);
        for(uint32_t x=0;x<ow;++x){double p=(x+.5)*w/ow-.5;xl[x]=int(std::floor(p));xf[x]=float(p-xl[x]);}
        for(uint32_t y=0;y<oh;++y){double p=(y+.5)*h/oh-.5;yl[y]=int(std::floor(p));yf[y]=float(p-yl[y]);}
        GpuNativeSettings settings{w,h,100,0,64,true,false,{}};
        uint64_t allocations=0;
        for(unsigned frame=0;frame<6;++frame){ScopedComputeBackend scope(ComputeBackend::Gpu);
            check(gpuDecodeNative(planes,settings,native.pixels),"Stable native reconstruction declined.");
            check(gpuContourUpscale(native,ow,oh,displayed,xl,xf,yl,yf,.8f,.8f,.3f,.27f,{},false),"Stable native/display chain declined.");
            check(samples==saved,"Native decode modified host coefficients.");
            if(frame==1){allocations=gpuActivity().bufferAllocations;expected=displayed;}
            if(frame>1)check(displayed==expected&&gpuActivity().bufferAllocations==allocations,"Stable native/display geometry reallocates buffers or changes pixels.");
        }
    }
    // Nearby geometries reuse physical capacity, but copy only logical bytes.
    // In particular, a larger staging buffer must never overwrite host guards.
    {
        uint64_t allocations=0;
        for(unsigned pass=0;pass<3;++pass){
            for(auto [w,h]:{std::pair{701u,513u},std::pair{641u,449u},std::pair{653u,457u},std::pair{700u,500u}}){
                Bytes input(pixelBytes(w,h),255);for(size_t at=0;at<input.size();at+=4)for(unsigned c=0;c<3;++c)input[at+c]=uint8_t(rng());
                auto expected=input;finalSharpen(expected,w,h,3,50);
                Bytes guarded(input.size()+32,197);std::copy(input.begin(),input.end(),guarded.begin()+16);
                const auto before=gpuActivity();
                {ScopedComputeBackend scope(ComputeBackend::Gpu);check(gpuFinalSharpen(std::span(guarded).subspan(16,input.size()),w,h,50),"Capacity-reuse sharpening declined.");}
                const auto after=gpuActivity();
                check(std::equal(expected.begin(),expected.end(),guarded.begin()+16),"Capacity reuse changed pixels after a geometry change.");
                check(std::all_of(guarded.begin(),guarded.begin()+16,[](auto value){return value==197;})&&std::all_of(guarded.end()-16,guarded.end(),[](auto value){return value==197;}),"Capacity readback overwrote host guards.");
                check(after.uploadedBytes-before.uploadedBytes==input.size()&&after.downloadedBytes-before.downloadedBytes==input.size(),"Capacity padding was transferred to or from the host.");
            }
            if(pass==1)allocations=gpuActivity().bufferAllocations;
            if(pass==2)check(gpuActivity().bufferAllocations==allocations,"Nearby image geometries reallocate warm GPU buffers.");
        }
    }
    // Native sparse uploads must preserve the established GPU inverse/color
    // arithmetic exactly, including signed zero and independent chroma padding.
    for(unsigned sampling:{420u,422u,444u})for(unsigned pattern:{0u,1u,2u}){
        constexpr uint32_t width=513,height=517,w=576,h=576;
        const uint32_t cw=sampling==444?w:320,ch=sampling==420?320:h;
        std::array<std::vector<float>,3> samples{std::vector<float>(w*h),std::vector<float>(cw*ch),std::vector<float>(cw*ch)};
        for(auto& p:samples){if(pattern==1){p[0]=-.0f;for(size_t i=1;i<p.size();i+=997)p[i]=float(int(i%129)-64)/16;}
            if(pattern==2)for(size_t i=0;i<p.size();++i)p[i]=.125f+float(i%31)/128;}
        const auto saved=samples;std::array<GpuPlane,3> planes{{{samples[0],w,h},{samples[1],cw,ch},{samples[2],cw,ch}}};
        GpuNativeSettings settings{width,height,100,8,64,sampling==420,sampling==422,{},0,4,2,3};
        Bytes dense(pixelBytes(width,height)),guarded(dense.size()+32,197);auto actual=std::span(guarded).subspan(16,dense.size());
        ScopedComputeBackend scope(ComputeBackend::Gpu);
        check(gpuDecodeNative(planes,settings,dense,false),"Dense native oracle declined.");
        const auto before=gpuActivity();check(gpuDecodeNative(planes,settings,actual),"Sparse native reconstruction declined.");const auto after=gpuActivity();
        check(std::equal(dense.begin(),dense.end(),actual.begin())&&samples==saved,"Sparse native upload changed pixels or host coefficients.");
        check(std::all_of(guarded.begin(),guarded.begin()+16,[](auto v){return v==197;})&&std::all_of(guarded.end()-16,guarded.end(),[](auto v){return v==197;}),"Sparse native output guard changed.");
        size_t bytes=0;for(const auto& p:saved)for(float v:p)bytes+=std::bit_cast<uint32_t>(v)!=0?8:0;
        if(pattern<2)check(after.sparseCoefficientUploads==before.sparseCoefficientUploads+1&&after.uploadedBytes-before.uploadedBytes==bytes,"Native sparse transfer retained zeros or changed negative zero.");
        else check(after.sparseCoefficientUploads==before.sparseCoefficientUploads,"Dense native data was packed sparsely.");
        auto malformed=planes;malformed[1].pixels={};bool rejected=false;
        try{gpuDecodeNative(malformed,settings,actual);}catch(const std::exception&){rejected=true;}check(rejected,"Empty native plane was not rejected before sparse packing.");
    }
    // Forward preparation consumes the exact retained GPU source, with partial
    // cells, independently padded chroma and only a small model upload.
    {
        constexpr uint32_t w=513,h=517,stride=576,ph=576,cw=320,ch=320,tile=64;
        Frame frame{w,h,0,1,Bytes(pixelBytes(w,h),255)};
        for(size_t at=0;at<frame.pixels.size();at+=4)for(int c=0;c<3;++c)frame.pixels[at+c]=uint8_t(rng());
        WaveletSourceColor source;for(auto& p:source.planes)p.resize(size_t(stride)*ph);
        std::array<GpuPlane,3> colors{{{source.planes[0],stride,ph},{source.planes[1],stride,ph},{source.planes[2],stride,ph}}};
        constexpr uint32_t cols=(w+tile-1)/tile,rows=(h+tile-1)/tile;
        std::vector<GpuColorStatistics> summaries(size_t(cols)*rows);
        ScopedComputeBackend scope(ComputeBackend::Gpu);
        if(gpuSourceColorModel(frame,colors,tile,summaries,{})){
            Bytes map(size_t(cols)*rows*2);for(auto& v:map)v=uint8_t(int8_t(int(rng()%49)-24));
            std::vector<float> means(size_t(cols)*rows);for(size_t i=0;i<means.size();++i)means[i]=float(summaries[i].sum/summaries[i].count);
            std::array<std::vector<float>,3> reference{source.planes[0],std::vector<float>(cw*ch),std::vector<float>(cw*ch)},result=reference;
            for(uint32_t y=0;y<ch;++y){
                std::array<std::vector<float>,3> model;for(auto& p:model)p.resize(cw);
                r6ModelRow(map,means,tile,cols,rows,float(std::min(h-1,y*2))+.5f,cw,2,.5f,model[0].data(),model[1].data(),model[2].data());
                for(uint32_t x=0;x<cw;++x){
                    const auto ax=std::min(w-1,x*2),bx=std::min(w-1,ax+1),ay=std::min(h-1,y*2),by=std::min(h-1,ay+1);
                    const auto& l=source.planes[0];
                    const float ac=((l[size_t(ay)*stride+ax]+l[size_t(ay)*stride+bx])+(l[size_t(by)*stride+ax]+l[size_t(by)*stride+bx]))*.25f-model[2][x];
                    for(int c=0;c<2;++c){const auto& p=source.planes[c+1];
                        reference[c+1][size_t(y)*cw+x]=((p[size_t(ay)*stride+ax]+p[size_t(ay)*stride+bx])+(p[size_t(by)*stride+ax]+p[size_t(by)*stride+bx]))*.25f-model[c][x]*ac;}
                }
            }
            for(int c=0;c<3;++c)waveletTransform(reference[c],c?cw:stride,c?ch:ph,3,false);
            std::array<GpuPlane,3> planes{{{result[0],stride,ph},{result[1],cw,ch},{result[2],cw,ch}}};
            const auto before=gpuActivity();check(gpuSourceWavelet(frame,planes,tile,map,means),"Retained source forward declined.");
            check(gpuActivity().uploadedBytes-before.uploadedBytes==means.size()*8,"Forward uploaded full CPU float planes.");
            for(int c=0;c<3;++c){double sum=0,maximum=0;for(size_t i=0;i<result[c].size();++i){double d=std::abs(result[c][i]-reference[c][i]);sum+=d;maximum=std::max(maximum,d);}
                std::cout<<"Resident forward plane="<<c<<" max="<<maximum<<" sum="<<sum<<'\n';
                check(maximum==0&&sum==0,"Resident source forward changed coefficients.");}
            const auto saved=result;
            {ScopedComputeBackend other(ComputeBackend::Gpu);check(!gpuSourceWavelet(frame,planes,tile,map,means)&&result==saved,"Forward reused another job's source.");}
        }
    }
    // Two immutable jobs interleave original/working RGBA at different sizes.
    // Restoring a job must hit its own GPU source, not upload or reuse another
    // frame. New jobs at a recycled address must always upload fresh pixels.
    {
        auto frame=[](uint32_t w,uint32_t h,uint8_t value){Frame f{w,h,0,1,Bytes(pixelBytes(w,h),255)};for(size_t at=0;at<f.pixels.size();at+=4){f.pixels[at]=value;f.pixels[at+1]=value/2;f.pixels[at+2]=255-value;}return f;};
        auto a=frame(512,512,41),aw=frame(512,640,73),b=frame(640,512,109),bw=frame(512,768,151);
        auto stats=[](const Frame& f){std::vector<GpuSourceStatistics> values(size_t((f.width+15)/16)*((f.height+15)/16));check(gpuSourceStatistics(f,16,0,values),"GPU source cache test declined.");return values;};
        auto same=[](const auto& a,const auto& b){return a.size()==b.size()&&!std::memcmp(a.data(),b.data(),a.size()*sizeof(GpuSourceStatistics));};
        std::vector<GpuSourceStatistics> original;
        {ScopedComputeBackend first(ComputeBackend::Gpu);original=stats(a);const auto working=stats(aw);
            {ScopedComputeBackend second(ComputeBackend::Gpu);const auto other=stats(b),otherWorking=stats(bw);const auto bytes=gpuActivity().uploadedBytes;
                check(same(other,stats(b))&&same(otherWorking,stats(bw)),"Interleaved GPU source cache returned another image.");
                check(gpuActivity().uploadedBytes==bytes,"Interleaved GPU sources were uploaded again.");}
            const auto bytes=gpuActivity().uploadedBytes;
            check(same(original,stats(a))&&same(working,stats(aw)),"Restored GPU source job changed statistics.");
            check(gpuActivity().uploadedBytes==bytes,"Restored GPU source job was uploaded again.");
        }
        for(size_t at=0;at<a.pixels.size();at+=4)a.pixels[at]=219;
        {ScopedComputeBackend next(ComputeBackend::Gpu);const auto bytes=gpuActivity().uploadedBytes;const auto changed=stats(a);
            check(!same(original,changed)&&gpuActivity().uploadedBytes==bytes+a.pixels.size(),"GPU source cache reused a recycled frame address.");}
    }
    // Backend scopes are independent across calling threads and restore on exit.
    {ScopedComputeBackend scope(ComputeBackend::Gpu);
        check(std::async(std::launch::async,[]{return computeBackend();}).get()==ComputeBackend::Cpu,"Backend leaked across threads.");}
    check(computeBackend()==ComputeBackend::Cpu,"Backend scope was not restored.");
    const auto a=gpuActivity();check(a.transforms>0&&a.resamples>0&&a.nativeDecodes>0&&a.analyses>0&&a.contourFilters>=8&&a.fallbacks==0,"GPU tests silently fell back to CPU.");
    std::cout<<"PASS R470 GPU: "<<a.transforms<<" transforms, "<<a.resamples<<" resamples, max transform error "<<worst<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
