#include "fajk/codec.hpp"
#include "fajk/media.hpp"
#include "../src/temporal_coefficients.hpp"
#include <fstream>
#include <iostream>
using namespace fajk;
int wmain(int argc,wchar_t** argv){try{
    require(argc>=4&&argc<=6,"coefficient_temporal_trial VIDEO OUTPUT COUNT [CAP_PERCENT=92] [PREVIOUS_FRAME=0]");MediaRuntime runtime;MediaSource source(argv[1]);const Path output=argv[2];std::filesystem::create_directories(output);const auto count=std::stoul(argv[3]);
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);EncodeOptions options;options.threads=10;options.speed=EncodeSpeed::Fast;options.backend=ComputeBackend::Gpu;options.targetBytes=80000;
    const auto cap=argc>4?std::stoul(argv[4]):92u;require(cap>=1&&cap<=100,"Invalid cap.");
    const bool previousFrame=argc>5&&std::stoul(argv[5])!=0;
    Packet key;Frame frame;uint64_t total=0,original=0;unsigned keys=0;std::ofstream csv(output/L"results.csv");csv<<"frame,bytes,intra_bytes,predicted_bands,pixels_exact\n";
    for(unsigned n=0;n<count&&source.next(frame);++n){const auto intra=codec->encode(frame,options);size_t bytes=intra.data.size();uint32_t bands=0;bool exact=true;
        if(n){auto candidate=encodeCoefficientTemporal(intra,key,10);if(candidate&&candidate->packet.data.size()*100<=intra.data.size()*cap){auto restored=expandCoefficientTemporal(candidate->packet.data,key,10);const auto a=codec->decode(intra,frame.width,frame.height),b=codec->decode(restored,frame.width,frame.height);exact=a.pixels==b.pixels;require(exact,"Coefficient temporal pixels differ.");bytes=candidate->packet.data.size();bands=candidate->predictedBands;}}
        if(!bands||previousFrame){key=intra;if(!bands)++keys;}total+=bytes;original+=intra.data.size();csv<<n<<','<<bytes<<','<<intra.data.size()<<','<<bands<<','<<exact<<'\n';csv.flush();std::cout<<n<<": "<<bytes<<" B, bands="<<bands<<", total="<<total<<" / "<<original<<", keys="<<keys<<std::endl;
    }return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
