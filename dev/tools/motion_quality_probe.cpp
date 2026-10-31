#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/media.hpp"
#include "fajk/entropy.hpp"
#include "../src/gpu_compute.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
using namespace fajk;
namespace {
// Opponent-color errors expose colored speckles that RGB MSE can hide under
// larger luminance/detail errors. Gradient RMS measures spatial variation of
// the error, subtracting real source edges rather than penalizing their color.
std::array<double,3> chromaQuality(const Frame& source,const Frame& decoded){
    auto error=[&](size_t at){
        const int r=int(decoded.pixels[at])-source.pixels[at],g=int(decoded.pixels[at+1])-source.pixels[at+1],b=int(decoded.pixels[at+2])-source.pixels[at+2];
        return std::array<double,3>{(r-b)*.5,(2*g-r-b)*.25,(r+2*g+b)*.25};
    };
    double square=0,gradient=0,lumaGradient=0;uint64_t pairs=0;
    for(uint32_t y=0;y<source.height;++y)for(uint32_t x=0;x<source.width;++x){
        const auto at=(size_t(y)*source.width+x)*4;const auto own=error(at);
        for(unsigned c=0;c<2;++c)square+=own[c]*own[c];
        for(auto offset:{x?size_t(4):size_t(0),y?size_t(source.width)*4:size_t(0)})if(offset){
            const auto other=error(at-offset);for(unsigned c=0;c<2;++c){const double d=own[c]-other[c];gradient+=d*d;}++pairs;
            const double dy=own[2]-other[2];lumaGradient+=dy*dy;
        }
    }
    return {square/(uint64_t(source.width)*source.height*2),pairs?std::sqrt(gradient/(pairs*2)):0.,pairs?std::sqrt(lumaGradient/pairs):0.};
}
// Keep the baseline on a dedicated worker so its GPU activity and CPU scratch
// stay separate. Decoder-owned reference caches also remain independent.
// Calls still execute sequentially; these times are not an isolated benchmark.
class BaselineDecoder {
    std::mutex mutex;std::condition_variable wake;bool closing=false;
    std::optional<std::packaged_task<Frame()>> pending;
    std::unique_ptr<FrameCodec> codec;
    std::jthread worker;
public:
    explicit BaselineDecoder(ComputeBackend backend):codec(makeCodec(CodecId::Fajk)){
        codec->setThreads(10);codec->setComputeBackend(backend);
        worker=std::jthread([this]{for(;;){std::optional<std::packaged_task<Frame()>> work;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return closing||pending.has_value();});
                if(closing)return;work=std::move(pending);pending.reset();}
            (*work)();
        }});
    }
    ~BaselineDecoder(){{std::lock_guard lock(mutex);closing=true;}wake.notify_one();}
    Frame decode(Packet packet,uint32_t width,uint32_t height){
        std::packaged_task<Frame()> work([this,packet=std::move(packet),width,height]{return codec->decode(packet,width,height);});
        auto future=work.get_future();{std::lock_guard lock(mutex);require(!pending,"Overlapping baseline request.");pending=std::move(work);}
        wake.notify_one();return future.get();
    }
};
}
int wmain(int argc,wchar_t** argv){try{
    require(argc>=7&&argc<=9,"motion_quality_probe VIDEO INTRA MAX OUTPUT COUNT BACKEND [SNAPSHOTS=1] [MOTION_FLICKER=0]");
    const bool snapshots=argc<8||std::stoul(argv[7])!=0;MediaRuntime runtime;MediaSource source(argv[1]);SequenceReader intra(argv[2]),motion(argv[3]);
    const bool motionFlicker=argc>=9&&std::stoul(argv[8])!=0;
    const Path output=argv[4];std::filesystem::create_directories(output);const auto count=uint32_t(std::stoul(argv[5]));require(count>0&&count<=intra.info().count&&count<=motion.info().count,"Invalid probe count.");
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);codec->setComputeBackend(std::stoul(argv[6])?ComputeBackend::Gpu:ComputeBackend::Cpu);
    BaselineDecoder baseline(std::stoul(argv[6])?ComputeBackend::Gpu:ComputeBackend::Cpu);
    std::ofstream csv(output/L"quality.csv");csv<<"frame,intra_bytes,max_bytes,mse_intra,mse_max,flicker_intra,flicker_max,stable_samples,intra_ms,max_ms,motion_flicker_intra,motion_flicker_max,motion_stable_samples,chroma_mse_intra,chroma_mse_max,chroma_error_gradient_intra,chroma_error_gradient_max,luma_error_gradient_intra,luma_error_gradient_max\n";
    Frame input,oldInput,oldIntra,oldMax;std::vector<uint32_t> hashes(count);uint64_t baselineBytes=0,motionBytes=0;
    for(uint32_t n=0;n<count;++n){require(source.next(input),"Short source.");const auto a=intra.read(n),b=motion.read(n);require(a.pts==b.pts&&a.duration==b.duration,"Timestamp mismatch.");
        auto begin=std::chrono::steady_clock::now();auto ia=baseline.decode(a,input.width,input.height);const double im=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();begin=std::chrono::steady_clock::now();auto ib=codec->decode(b,input.width,input.height);const double mm=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();hashes[n]=crc32(ib.pixels);
        double mseA=0,mseB=0,flickerA=0,flickerB=0;uint64_t stable=0;
        for(size_t i=0;i<input.pixels.size();i+=4){unsigned change=0;if(n)for(unsigned c=0;c<3;++c)change=std::max(change,unsigned(std::abs(int(input.pixels[i+c])-int(oldInput.pixels[i+c]))));
            for(unsigned c=0;c<3;++c){const double x=int(ia.pixels[i+c])-int(input.pixels[i+c]),y=int(ib.pixels[i+c])-int(input.pixels[i+c]);mseA+=x*x;mseB+=y*y;
                if(n&&change<=3){const int signal=int(input.pixels[i+c])-int(oldInput.pixels[i+c]),fa=int(ia.pixels[i+c])-int(oldIntra.pixels[i+c])-signal,fb=int(ib.pixels[i+c])-int(oldMax.pixels[i+c])-signal;flickerA+=fa*fa;flickerB+=fb*fb;++stable;}}}
        double alignedA=0,alignedB=0;uint64_t alignedSamples=0;
        if(n&&motionFlicker){
            // Same-pixel flicker misses moving subjects. Estimate displacement
            // from the two original frames, then align both decoded outputs
            // with exactly that same field. Only reliably matched source
            // pixels contribute; disocclusions are not mistaken for flicker.
            ScopedComputeBackend compute(ComputeBackend::Gpu);
            std::vector<uint32_t> vectors(size_t((input.width+31)/32)*((input.height+31)/32)),affine(vectors.size());
            require(gpuBlockMotion(input,oldInput,vectors,affine),"Motion flicker requires a supported GPU.");
            Frame alignedSource{input.width,input.height,0,1,Bytes(input.pixels.size())},alignedIntra=alignedSource,alignedMax=alignedSource;
            const std::array<int,3> bias{};
            require(gpuMotionWarp(oldInput,32,vectors,bias,{},alignedSource.pixels,true,affine)&&gpuMotionWarp(oldIntra,32,vectors,bias,{},alignedIntra.pixels,true,affine)&&gpuMotionWarp(oldMax,32,vectors,bias,{},alignedMax.pixels,true,affine),"Motion flicker alignment failed.");
            for(size_t i=0;i<input.pixels.size();i+=4){unsigned change=0;for(unsigned c=0;c<3;++c)change=std::max(change,unsigned(std::abs(int(input.pixels[i+c])-int(alignedSource.pixels[i+c]))));
                if(change<=3)for(unsigned c=0;c<3;++c){const int signal=int(input.pixels[i+c])-int(alignedSource.pixels[i+c]),fa=int(ia.pixels[i+c])-int(alignedIntra.pixels[i+c])-signal,fb=int(ib.pixels[i+c])-int(alignedMax.pixels[i+c])-signal;alignedA+=fa*fa;alignedB+=fb*fb;++alignedSamples;}}
        }
        const auto ca=chromaQuality(input,ia),cb=chromaQuality(input,ib);
        csv<<n<<','<<a.data.size()<<','<<b.data.size()<<','<<mseA/(input.width*input.height*3ull)<<','<<mseB/(input.width*input.height*3ull)<<','<<(stable?std::sqrt(flickerA/stable):0)<<','<<(stable?std::sqrt(flickerB/stable):0)<<','<<stable<<','<<im<<','<<mm<<','<<(alignedSamples?std::sqrt(alignedA/alignedSamples):0)<<','<<(alignedSamples?std::sqrt(alignedB/alignedSamples):0)<<','<<alignedSamples<<','<<ca[0]<<','<<cb[0]<<','<<ca[1]<<','<<cb[1]<<','<<ca[2]<<','<<cb[2]<<'\n';csv.flush();
        if(snapshots&&(n==0||n==1||n==12||n==23||n==47||n==119||n+1==count)){const auto name=std::to_string(n);writePng(output/(name+"-source.png"),input);writePng(output/(name+"-intra.png"),ia);writePng(output/(name+"-max.png"),ib);}
        baselineBytes+=a.data.size();motionBytes+=b.data.size();oldInput=input;oldIntra=std::move(ia);oldMax=std::move(ib);
    }
    // Fresh reader, reverse/out-of-order access. The cache must not influence
    // canonical references or make seeks produce a different noise pattern.
    SequenceReader random(argv[3]);for(uint64_t n:{uint64_t(count-1),uint64_t(1),uint64_t(0),uint64_t(count/2),uint64_t(count-1)}){
        const auto image=codec->decode(random.read(n),input.width,input.height);require(crc32(image.pixels)==hashes[size_t(n)],"Random seek differs from sequential presentation.");}
    std::cout<<"PASS "<<count<<" frames, deterministic random seeks, payload "<<motionBytes<<" / "<<baselineBytes<<" B, savings "<<100*(1.-double(motionBytes)/baselineBytes)<<"%\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
