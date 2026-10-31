#include "fajk/media.hpp"
#include "fajk/codec.hpp"
#include "../src/temporal_predictive.hpp"
#include <fstream>
#include <iostream>
using namespace fajk;
int wmain(int argc,wchar_t** argv){try{
    require(argc>=4&&argc<=8,"predictive_sequence_trial VIDEO OUTPUT STEP [KEY_BYTES=80000] [COUNT=48] [CAP_PERCENT=90] [TOLERANCE=3]");MediaRuntime runtime;const Path output=argv[2];std::filesystem::create_directories(output);const auto step=uint32_t(std::stoul(argv[3]));
    const auto keyBytes=argc>4?uint32_t(std::stoul(argv[4])):80000u,count=argc>5?uint32_t(std::stoul(argv[5])):48u,capPercent=argc>6?uint32_t(std::stoul(argv[6])):90u;
    require(keyBytes>=1024&&keyBytes<=maxPacket&&count>=1&&count<=10000&&capPercent>=1&&capPercent<=100,"Invalid trial options.");
    const auto tolerance=argc>7?uint32_t(std::stoul(argv[7])):3u;require(tolerance<=16,"Invalid tolerance.");
    MediaSource media(argv[1]);EncodeOptions options;options.backend=ComputeBackend::Gpu;options.speed=EncodeSpeed::Fast;options.threads=10;options.targetBytes=80000;
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);codec->setComputeBackend(ComputeBackend::Cpu);
    std::ofstream csv(output/L"results.csv");csv<<"frame,predicted,bytes,intra_bytes,retained,blocks,flicker,flicker_intra,stable_pixels,candidate_bytes,motion_ms,transform_ms,coding_ms\n";
    Frame source,previousSource,previousIntra,witness,reference,previousResult;Bytes ages;uint64_t sum=0,intraSum=0;unsigned keys=0;
    for(uint32_t n=0;n<count&&media.next(source);++n){
        const auto intra=codec->encode(source,options);const auto ordinary=codec->decode(intra,source.width,source.height);Frame reconstructed;size_t bytes=intra.data.size();bool predicted=false;uint32_t kept=0,blocks=0;
        size_t candidate=0;double motion=0,transform=0,coding=0;
        if(n){auto trial=trialPredictive(source,witness,reference,10,tolerance,step,ComputeBackend::Gpu,ages,10);kept=trial.retained;blocks=trial.blocks;candidate=trial.packet.data.size();motion=trial.motionMs;transform=trial.transformMs;coding=trial.codingMs;
            if(trial.packet.data.size()<=uint64_t(options.targetBytes)*capPercent/100){bytes=trial.packet.data.size();reconstructed=std::move(trial.image);witness=std::move(trial.witness);ages=std::move(trial.ages);predicted=true;}}
        if(!predicted){if(keyBytes==options.targetBytes)reconstructed=ordinary;else {auto keyOptions=options;keyOptions.targetBytes=keyBytes;const auto key=codec->encode(source,keyOptions);bytes=key.data.size();reconstructed=codec->decode(key,source.width,source.height);}witness=source;ages.clear();++keys;}
        double flicker=0,oldFlicker=0;uint64_t stable=0;
        if(n)for(size_t i=0;i<source.pixels.size();i+=4){unsigned max=0;for(int c=0;c<3;++c)max=std::max(max,unsigned(std::abs(int(source.pixels[i+c])-int(previousSource.pixels[i+c]))));
            if(max<=3)for(int c=0;c<3;++c){const int signal=int(source.pixels[i+c])-int(previousSource.pixels[i+c]);const int e=int(reconstructed.pixels[i+c])-int(previousResult.pixels[i+c])-signal;const int o=int(ordinary.pixels[i+c])-int(previousIntra.pixels[i+c])-signal;flicker+=e*e;oldFlicker+=o*o;++stable;}}
        sum+=bytes;intraSum+=intra.data.size();csv<<n<<','<<predicted<<','<<bytes<<','<<intra.data.size()<<','<<kept<<','<<blocks<<','<<(stable?std::sqrt(flicker/stable):0)<<','<<(stable?std::sqrt(oldFlicker/stable):0)<<','<<stable/3<<','<<candidate<<','<<motion<<','<<transform<<','<<coding<<'\n';csv.flush();
        if(n==0||n==1||n==5||n==12||n==23||n==47){const auto name=std::to_string(n);writePng(output/(name+"-source.png"),source);writePng(output/(name+"-intra.png"),ordinary);writePng(output/(name+"-predicted.png"),reconstructed);}
        reference=reconstructed;previousResult=std::move(reconstructed);previousSource=source;previousIntra=ordinary;
        std::cout<<n<<": "<<bytes<<" B, P="<<predicted<<", total="<<sum<<" / "<<intraSum<<", keys="<<keys<<std::endl;
    }return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
