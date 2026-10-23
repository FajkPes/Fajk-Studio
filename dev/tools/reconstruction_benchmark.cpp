#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/media.hpp"
#include "fajk/pipeline.hpp"
#include "../src/reconstruction441.hpp"
#include "../src/reconstruction441_gpu.hpp"
#include "../src/coherent_reference441.hpp"
#include "../src/decode_internal.hpp"
#include "../src/cleanup_quality.hpp"
#include "../src/tangent_cleanup.hpp"
#include "../src/timing.hpp"
#include "../src/flat_regions.hpp"
#include "../src/resample.hpp"
#include "../src/fast_reconstruction.hpp"
#include "../src/edge_upscale.hpp"
#include <chrono>
#include <iostream>
#include <fstream>
using namespace fajk;
int wmain(int argc,wchar_t** argv) {try {
    require(argc>=4,"reconstruction_benchmark INPUT PREFIX cpu|gpu|off [REPEATS] [TANGENTS] [WORKERS]");
    MediaRuntime runtime;SequenceReader reader(argv[1]);auto p=reader.read(0);auto info=reader.info();
    auto view=read441(p.data,info.width,info.height);auto settings=view.settings;
    if(argc>5)settings.strongerTangents=std::stoul(argv[5]);
    uint32_t workers=argc>6?std::stoul(argv[6]):0;int count=argc>4?std::stoi(argv[4]):3;
    std::wstring mode=argv[3];const bool directional=mode==L"directional"||mode==L"directional-tangent";
    require(directional||mode==L"cpu"||mode==L"recursive"||mode==L"native"||mode==L"compact8"||mode==L"compact"||mode==L"cheap"||mode==L"gpu"||mode==L"off"||mode==L"fused"||mode==L"unified","Invalid mode");
    std::ofstream csv(Path(std::wstring(argv[2])+L".csv"));csv<<"repeat,base_ms,cleanup_ms,aa_text_ms,total_ms\n";
    auto ms=[](auto start){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();};
    Frame image;reconstruction441GpuDiagnostics(true);
    for(int repeat=-1;repeat<count;++repeat){
        std::vector<TimingEvent> events;TimingCapture capture(&events);
        auto start=std::chrono::steady_clock::now();
        DeferredUpscale deferred;deferred.skipBaseCleanup=directional||mode==L"unified"||mode==L"native"||mode==L"recursive";
        if(directional||mode==L"fused"||mode==L"unified"||mode==L"native"||mode==L"recursive")image=decodeFrameDeferred({view.child,p.pts,p.duration},info.width,info.height,workers,deferred);
        else image=decodeFrameWithSettings({view.child,p.pts,p.duration},info.width,info.height,workers,100,0,true);
        double base=ms(start);auto t=std::chrono::steady_clock::now();
        Bytes original;if(settings.protectText&&(mode==L"cpu"||mode==L"cheap"||mode==L"compact"||mode==L"compact8"))original=image.pixels;
        auto q=cleanupQuality(view.budget,info.width,info.height);
        float amount=std::clamp(q.contours*settings.intensity/100.f,0.f,1.f);
        if(directional){
            require(deferred.active,"This packet is not scaled");
            const auto before=image.pixels;
            recursiveContourCleanup(image.pixels,image.width,image.height,workers,amount,settings.strongerTangents/100.f,before);
            if(mode==L"directional-tangent")tangentCleanup(image.pixels,image.width,image.height,workers,.7f*amount,2.5f,40);
            if(settings.protectText)preserveDecodedText(image.pixels,before,image.width,image.height,workers);
            image=edgeUpscale(image,info.width,info.height,workers,{},amount,false,false);
            if(!deferred.flatMetadata.empty()){auto flat=readFlatRegions(deferred.flatMetadata,info.width,info.height);applyFlatRegions(image.pixels,flat,info.width,info.height,workers);}
        }
        else if(mode==L"recursive"){
            require(deferred.active,"This packet is not scaled");image=fastReconstruct441(image,info.width,info.height,workers,view.budget,settings);
            if(!deferred.flatMetadata.empty()){auto flat=readFlatRegions(deferred.flatMetadata,info.width,info.height);applyFlatRegions(image.pixels,flat,info.width,info.height,workers);}
        }
        else if(mode==L"native"){
            require(deferred.active,"This packet is not scaled");
            Bytes before;if(settings.protectText)before=image.pixels;
            compactTangentCleanup(image.pixels,image.width,image.height,workers,amount,8,settings.style==0?.08f:0.f,settings.strongerTangents/100.f);
            if(settings.protectText)preserveDecodedText(image.pixels,before,image.width,image.height,workers);
            image=resamplePhoto(image,info.width,info.height,workers,{},2);
            if(!deferred.flatMetadata.empty()){auto flat=readFlatRegions(deferred.flatMetadata,info.width,info.height);applyFlatRegions(image.pixels,flat,info.width,info.height,workers);}
        }
        else if(mode==L"compact8")compactTangentCleanup(image.pixels,info.width,info.height,workers,amount,8,settings.style==0?.08f:0.f,settings.strongerTangents/100.f);
        else if(mode==L"compact")compactTangentCleanup(image.pixels,info.width,info.height,workers,amount,4);
        else if(mode==L"cheap"){
            tangentCleanup(image.pixels,info.width,info.height,workers,.65f*amount,5,20);
            tangentCleanup(image.pixels,info.width,info.height,workers,.65f*amount,5,20);
        }else if(mode==L"cpu") {
            coherent441::Image input(size_t(info.width)*info.height);
            for(size_t i=0;i<input.size();++i)for(int c=0;c<3;++c)input[i][c]=image.pixels[i*4+c];
            auto clean=coherent441::monotone(input,info.width,info.height,workers,.65f,settings.style==0?.08f:0.f);
            if(repeat==0)std::cout<<"CPU skirt (including conversion) "<<ms(t)<<"\n";
            auto complete=coherent441::coherent(clean,info.width,info.height,workers,5,20,4,1,.6f,settings.strongerTangents/100.f);
            parallelFor(info.height,workers,[&](uint32_t y){for(uint32_t x=0;x<info.width;++x){auto i=size_t(y)*info.width+x;for(int c=0;c<3;++c)image.pixels[i*4+c]=uint8_t(std::clamp(int(input[i][c]+amount*(complete[i][c]-input[i][c])+.5f),0,255));}});
        }else if(mode==L"fused"||mode==L"unified"){
            require(deferred.active,"This packet is not scaled");
            FlatRegionLayout flat;if(!deferred.flatMetadata.empty())flat=readFlatRegions(deferred.flatMetadata,info.width,info.height);
            Frame output{info.width,info.height,image.pts,image.duration,Bytes(pixelBytes(info.width,info.height))};
            require(reconstruct441GpuScaled(image.pixels,image.width,image.height,output.pixels,info.width,info.height,std::min(1.f,deferred.contours/100.f),deferred.flatMetadata.empty()?nullptr:&flat,
                amount,settings.strongerTangents/100.f,settings.style==0?.08f:0.f,q.antialias*settings.antialias/100.f,q.antialias*settings.contrast/100.f,settings.protectText),"GPU unavailable");
            image=std::move(output);
        }else if(mode==L"gpu")require(reconstruct441Gpu(image.pixels,info.width,info.height,amount,settings.strongerTangents/100.f,settings.style==0?.08f:0.f,q.antialias*settings.antialias/100.f,q.antialias*settings.contrast/100.f,settings.protectText),"GPU unavailable");
        double cleanup=ms(t);t=std::chrono::steady_clock::now();
        if(mode==L"cpu"||mode==L"cheap"||mode==L"compact"||mode==L"compact8"){
            if(q.antialias&&settings.antialias)contourAntialias(image.pixels,info.width,info.height,workers,q.antialias*settings.antialias/100.f);
            if(q.antialias&&settings.contrast)boundedContourContrast(image.pixels,info.width,info.height,workers,q.antialias*settings.contrast/100.f);
            if(settings.protectText)preserveDecodedText(image.pixels,original,info.width,info.height,workers);
        }
        double tail=ms(t),total=ms(start);
        std::cout<<repeat<<" base "<<base<<" cleanup "<<cleanup<<" tail "<<tail<<" total "<<total<<"\n";
        if(repeat>=0)csv<<repeat<<','<<base<<','<<cleanup<<','<<tail<<','<<total<<'\n';
        if(repeat==0)for(auto e:events)std::cout<<e.stage<<' '<<e.ms<<'\n';
    }
    std::cout<<"GPU: "<<reconstruction441GpuDevice()<<"\n"<<reconstruction441GpuProfile();
    writePng(Path(std::wstring(argv[2])+L".png"),image);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<" GPU: "<<reconstruction441GpuDevice()<<'\n';return 1;}}
