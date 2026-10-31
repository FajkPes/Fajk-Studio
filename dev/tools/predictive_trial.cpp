#include "fajk/media.hpp"
#include "fajk/codec.hpp"
#include "../src/temporal_predictive.hpp"
#include <fstream>
#include <iostream>
using namespace fajk;
int wmain(int argc,wchar_t** argv){try{
    require(argc==4,"predictive_trial VIDEO PHOTO OUTPUT");MediaRuntime runtime;const Path output=argv[3];std::filesystem::create_directories(output);
    MediaSource media(argv[1]);Frame first,next;require(media.next(first),"Missing first frame.");std::vector<Frame> frames{first};for(uint32_t n=1;n<24&&media.next(next);++n)frames.push_back(std::move(next));
    const auto photo=readImage(argv[2]);
    std::ofstream csv(output/L"results.csv");csv<<"scene,frame,step,tolerance,bytes,intra_bytes,retained,blocks,motion_ms,transform_ms,coding_ms\n";
    for(uint32_t scene=0;scene<2;++scene){const auto& base=scene?photo:first;EncodeOptions options;options.backend=ComputeBackend::Gpu;options.speed=EncodeSpeed::Fast;options.threads=10;options.targetBytes=80000;
        auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);codec->setComputeBackend(ComputeBackend::Cpu);
        for(uint32_t n:{1,12,23}){auto frame=scene?photo:frames.at(n);if(scene){for(size_t i=0;i<frame.pixels.size();i+=4)for(int c=0;c<3;++c)frame.pixels[i+c]=uint8_t(std::min(255,int(frame.pixels[i+c])+2));}
            const auto intra=codec->encode(frame,options);const auto ordinary=codec->decode(intra,base.width,base.height);const auto name=std::string(scene?"lighting":"motion")+"-"+std::to_string(n);
            const auto& reference=scene?photo:frames.at(n-1);auto keyOptions=options;keyOptions.targetBytes=320000;
            const auto key=codec->encode(reference,keyOptions);const auto decoded=codec->decode(key,base.width,base.height);
            writePng(output/(name+"-source.png"),frame);writePng(output/(name+"-intra.png"),ordinary);
            for(uint32_t step:{4,8,16,32,48}){const auto trial=trialPredictive(frame,reference,decoded,10,3,step,ComputeBackend::Gpu);
                writePng(output/(name+"-q"+std::to_string(step)+".png"),trial.image);
                csv<<(scene?"lighting":"motion")<<','<<n<<','<<step<<",3,"<<trial.packet.data.size()<<','<<intra.data.size()<<','<<trial.retained<<','<<trial.blocks<<','<<trial.motionMs<<','<<trial.transformMs<<','<<trial.codingMs<<'\n';csv.flush();
                std::cout<<name<<" q"<<step<<": "<<trial.packet.data.size()<<" B, retained "<<trial.retained<<'/'<<trial.blocks<<std::endl;
            }
        }
    }return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
