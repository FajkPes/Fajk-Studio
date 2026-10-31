#include "fajk/media.hpp"
#include "fajk/codec.hpp"
#include "../src/temporal_predictive.hpp"
#include "../src/reconstruction441.hpp"
#include "../src/decode_internal.hpp"
#include "../src/artifact_reconstruction.hpp"
#include "../src/fast_reconstruction.hpp"
#include <fstream>
#include <iostream>
using namespace fajk;
namespace {
Frame render(Frame raw,uint32_t width,uint32_t height,const DeferredUpscale& native,uint32_t budget,const Reconstruction441& settings){
    artifactReconstruction::restore(raw.pixels,raw.width,raw.height,10,native.budget,native.surfaces,native.contours,width,height);
    return fastReconstruct441(raw,width,height,10,budget,settings);
}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc>=4&&argc<=12,"native_predictive_sequence_trial VIDEO OUTPUT STEP [PROPAGATED_ERROR=0] [COUNT=48] [COMPACT=0] [CURVE=65] [CODED_TARGET=0] [BLOCK_TRANSFORM=0] [DIRECT_KEY=0] [GPU_BLOCKS=0]");MediaRuntime runtime;MediaSource media(argv[1]);const Path output=argv[2];std::filesystem::create_directories(output);const auto step=uint32_t(std::stoul(argv[3]));
    const auto propagatedError=argc>4?uint32_t(std::stoul(argv[4])):0u,count=argc>5?uint32_t(std::stoul(argv[5])):48u;
    require(count>=1&&count<=10000,"Invalid frame count.");
    const bool directKey=argc>10&&std::stoul(argv[10])!=0;
    const bool gpuBlocks=argc>11&&std::stoul(argv[11])!=0;
    const bool blockTransform=argc>9&&std::stoul(argv[9])!=0;
    const bool codedTarget=argc>8&&std::stoul(argv[8])!=0;
    const auto curve=argc>7?uint32_t(std::stoul(argv[7])):65u;
    const bool compact=argc>6&&std::stoul(argv[6])!=0;
    EncodeOptions options;options.backend=ComputeBackend::Gpu;options.speed=EncodeSpeed::Fast;options.threads=10;options.targetBytes=80000;
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);codec->setComputeBackend(ComputeBackend::Cpu);
    std::ofstream csv(output/L"results.csv");csv<<"frame,predicted,bytes,intra_bytes,candidate_bytes,retained,blocks,flicker,flicker_intra,stable_pixels,motion_ms,transform_ms,coding_ms,mse,mse_intra\n";
    Frame source,previousSource,previousIntra,witness,reference,previousResult;Bytes ages;DeferredUpscale native;uint32_t budget=0;Reconstruction441 settings;uint64_t sum=0,intraSum=0;unsigned keys=0;
    for(uint32_t n=0;n<count&&media.next(source);++n){
        const auto intra=codec->encode(source,options);const auto ordinary=codec->decode(intra,source.width,source.height);Frame raw;size_t bytes=intra.data.size(),candidate=0;bool predicted=false;uint32_t kept=0,blocks=0;double motion=0,transform=0,coding=0;
        if(n){Frame working;{ScopedComputeBackend gpu(ComputeBackend::Gpu);working=resamplePhoto(source,reference.width,reference.height,10,{},2);}
            Frame target;DeferredUpscale currentNative;
            if(codedTarget){const auto view=read441(intra.data,source.width,source.height);currentNative.skipBaseCleanup=true;
                target=decodeNestedFrame({view.child,source.pts,source.duration},source.width,source.height,10,{},&currentNative,100,0);
                require(currentNative.active&&target.width==working.width&&target.height==working.height,"Coded target dimensions differ.");}
            auto trial=trialPredictive(working,witness,reference,10,3,step,ComputeBackend::Gpu,ages,10,propagatedError,compact,curve,codedTarget?&target:nullptr,blockTransform,gpuBlocks,argc>11&&std::stoul(argv[11])==2);kept=trial.retained;blocks=trial.blocks;candidate=trial.packet.data.size();motion=trial.motionMs;transform=trial.transformMs;coding=trial.codingMs;
            if(gpuBlocks){const auto check=decodePredictiveTrial(trial.packet,reference,10);require(check.pixels==trial.image.pixels,"GPU block predictor differs from the independent CPU decode.");}
            if(candidate<=options.targetBytes){bytes=candidate;raw=std::move(trial.image);if(!directKey){witness=std::move(trial.witness);ages=std::move(trial.ages);}predicted=true;}}
        if(!predicted){const auto view=read441(intra.data,source.width,source.height);native={};native.skipBaseCleanup=true;
            raw=decodeNestedFrame({view.child,source.pts,source.duration},source.width,source.height,10,{},&native,100,0);
            require(native.active&&native.flatMetadata.empty()&&!view.settings.protectText,"Unsupported native trial key.");budget=view.budget;settings=view.settings;
            {ScopedComputeBackend gpu(ComputeBackend::Gpu);witness=resamplePhoto(source,raw.width,raw.height,10,{},2);}ages.clear();++keys;}
        const auto reconstructed=render(raw,source.width,source.height,native,budget,settings);
        if(!predicted)require(reconstructed.pixels==ordinary.pixels,"Raw key rendering differs from ordinary decode.");
        double mse=0,intraMse=0;for(size_t i=0;i<source.pixels.size();i+=4)for(unsigned c=0;c<3;++c){const double a=int(source.pixels[i+c])-int(reconstructed.pixels[i+c]),b=int(source.pixels[i+c])-int(ordinary.pixels[i+c]);mse+=a*a;intraMse+=b*b;}
        double flicker=0,oldFlicker=0;uint64_t stable=0;
        if(n)for(size_t i=0;i<source.pixels.size();i+=4){unsigned maximum=0;for(int c=0;c<3;++c)maximum=std::max(maximum,unsigned(std::abs(int(source.pixels[i+c])-int(previousSource.pixels[i+c]))));if(maximum<=3)for(int c=0;c<3;++c){const int signal=int(source.pixels[i+c])-int(previousSource.pixels[i+c]),a=int(reconstructed.pixels[i+c])-int(previousResult.pixels[i+c])-signal,b=int(ordinary.pixels[i+c])-int(previousIntra.pixels[i+c])-signal;flicker+=a*a;oldFlicker+=b*b;++stable;}}
        sum+=bytes;intraSum+=intra.data.size();csv<<n<<','<<predicted<<','<<bytes<<','<<intra.data.size()<<','<<candidate<<','<<kept<<','<<blocks<<','<<(stable?std::sqrt(flicker/stable):0)<<','<<(stable?std::sqrt(oldFlicker/stable):0)<<','<<stable/3<<','<<motion<<','<<transform<<','<<coding<<','<<mse/(source.pixels.size()/4*3)<<','<<intraMse/(source.pixels.size()/4*3)<<'\n';csv.flush();
        if(n==0||n==1||n==5||n==12||n==23||n==47){const auto name=std::to_string(n);writePng(output/(name+"-source.png"),source);writePng(output/(name+"-intra.png"),ordinary);writePng(output/(name+"-predicted.png"),reconstructed);}
        if(!directKey||!predicted)reference=std::move(raw);previousResult=reconstructed;previousSource=source;previousIntra=ordinary;
        std::cout<<n<<": "<<bytes<<" B, P="<<predicted<<", total="<<sum<<" / "<<intraSum<<", keys="<<keys<<std::endl;
    }return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
