#include "fajk/codec.hpp"
#include "fajk/media.hpp"
#include "fajk/hardware.hpp"
#include "../src/gpu_compute.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iomanip>
using namespace fajk;
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
struct Quality {
    double rgb=0,red=0,edge=0,redEdge=0,flatChroma=0;
    uint64_t redCount=0,edgeCount=0,redEdgeCount=0,flatCount=0;
};
Quality measure(const Frame& source,const Frame& image){
    Quality q;const auto w=source.width,h=source.height;
    auto pixel=[&](uint32_t x,uint32_t y){return source.pixels.data()+(size_t(y)*w+x)*4;};
    auto red=[](const uint8_t* p){return p[0]>70&&p[0]>p[1]+24&&p[0]>p[2]+16;};
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
        const auto at=(size_t(y)*w+x)*4;const auto* p=pixel(x,y);const auto* a=image.pixels.data()+at;
        double error=0;for(int c=0;c<3;++c){const double d=int(a[c])-p[c];error+=d*d;}error/=3;
        const auto* left=pixel(x?x-1:x,y);const auto* right=pixel(std::min(w-1,x+1),y);
        const auto* up=pixel(x,y?y-1:y);const auto* down=pixel(x,std::min(h-1,y+1));
        int gradient=0;for(int c=0;c<3;++c)gradient+=std::abs(int(right[c])-left[c])+std::abs(int(down[c])-up[c]);
        const bool isRed=red(p),nearRed=isRed||red(left)||red(right)||red(up)||red(down);
        q.rgb+=error;if(isRed){q.red+=error;++q.redCount;}
        if(gradient>=64){q.edge+=error;++q.edgeCount;if(nearRed){q.redEdge+=error;++q.redEdgeCount;}}
        if(gradient<=6&&x&&y){
            const auto* al=a-4;const auto* au=a-size_t(w)*4;
            const int co=int(a[0])-a[2]-(int(p[0])-p[2]),cg=2*int(a[1])-a[0]-a[2]-(2*int(p[1])-p[0]-p[2]);
            for(auto [b,s]:{std::pair{al,left},std::pair{au,up}}){
                const int dco=co-(int(b[0])-b[2]-(int(s[0])-s[2]));
                const int dcg=cg-(2*int(b[1])-b[0]-b[2]-(2*int(s[1])-s[0]-s[2]));
                q.flatChroma+=(4.*dco*dco+double(dcg)*dcg)/6;++q.flatCount;
            }
        }
    }
    q.rgb/=uint64_t(w)*h;q.red/=std::max(uint64_t(1),q.redCount);q.edge/=std::max(uint64_t(1),q.edgeCount);
    q.redEdge/=std::max(uint64_t(1),q.redEdgeCount);q.flatChroma/=std::max(uint64_t(1),q.flatCount);return q;
}
Frame synthetic(){
    Frame f{2560,1440,0,1,Bytes(pixelBytes(2560,1440),255)};
    for(uint32_t y=0;y<f.height;++y)for(uint32_t x=0;x<f.width;++x){
        auto* p=f.pixels.data()+(size_t(y)*f.width+x)*4;
        const int cx=int(x%640)-320,cy=int(y%480)-240;
        const int jitter=int(7*std::sin(y*.025));
        const bool shape=cx*cx+cy*cy<140*140||std::abs(cx+cy/3-jitter)<3;
        const unsigned panel=x/640;
        const int low[3]={42,45,49},high[4][3]={{220,28,35},{32,205,50},{36,55,220},{205,190,30}};
        for(int c=0;c<3;++c)p[c]=uint8_t(shape?high[panel][c]:low[c]+int(y/80)%3);
    }return f;
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==4,"fast_max_benchmark INPUT.png|synthetic OUTPUT_DIRECTORY cpu|gpu");
    const auto source=std::wstring(argv[1])==L"synthetic"?synthetic():readImage(argv[1]);
    const Path directory=argv[2];std::filesystem::create_directories(directory);
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);codec->setSharpening(50);
    const auto backend=std::wstring(argv[3])==L"gpu"?ComputeBackend::Gpu:ComputeBackend::Cpu;codec->setComputeBackend(backend);
    std::ofstream log(directory/L"results.csv");log<<std::setprecision(12);
    log<<"budget,mode,round,bytes,encode_ms,decode_ms,rgb_mse,red_mse,edge_mse,red_edge_mse,flat_chroma_noise,red_pixels,red_edge_pixels,gpu_failures\n";
    for(uint32_t budget:{30000u,80000u}){
        EncodeOptions options;options.targetBytes=budget;options.quality=100;options.backend=backend;options.threads=10;
        options.reconstruction441.intensity=50;
        for(auto speed:{EncodeSpeed::Fast,EncodeSpeed::MaxCompression}){options.speed=speed;const auto p=codec->encode(source,options);codec->decode(p,source.width,source.height);}
        for(int round=0;round<4;++round)for(int i=0;i<2;++i){
            const bool max=(i^(round&1))!=0;options.speed=max?EncodeSpeed::MaxCompression:EncodeSpeed::Fast;
            const auto activity=gpuActivity();auto start=Clock::now();auto packet=codec->encode(source,options);const auto encodeMs=elapsed(start);
            Frame decoded;start=Clock::now();for(int n=0;n<4;++n)decoded=codec->decode(packet,source.width,source.height);const auto decodeMs=elapsed(start)/4;
            require(packet.data.size()<=budget,"Mode exceeded its budget");const auto q=measure(source,decoded);
            log<<budget<<','<<(max?"max":"fast")<<','<<round<<','<<packet.data.size()<<','<<encodeMs<<','<<decodeMs<<','
               <<q.rgb<<','<<q.red<<','<<q.edge<<','<<q.redEdge<<','<<q.flatChroma<<','<<q.redCount<<','<<q.redEdgeCount<<','<<gpuActivity().fallbacks-activity.fallbacks<<'\n';
            if(!round){const auto stem=std::to_wstring(budget)+(max?L"-max":L"-fast");writePng(directory/(stem+L".png"),decoded);}
        }
    }
    writePng(directory/L"source.png",source);std::cout<<"PASS same-cap quality and timing, red masks are source-only; hash/metrics/PNG excluded from timers.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
