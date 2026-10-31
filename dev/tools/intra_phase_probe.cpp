#include "fajk/media.hpp"
#include "fajk/codec.hpp"
#include "fajk/entropy.hpp"
#include "../src/wavelet.hpp"
#include "../src/wavelet_rate_control.hpp"
#include <fstream>
#include <iostream>
using namespace fajk;
int wmain(int argc,wchar_t** argv){try{
    require(argc==3,"intra_phase_probe VIDEO OUTPUT");MediaRuntime runtime;MediaSource media(argv[1]);Frame source;require(media.next(source),"Empty input.");
    require(source.width>768&&source.height>768,"Phase probe needs a sufficiently large image.");
    const Path output=argv[2];std::filesystem::create_directories(output);
    EncodeOptions options;options.backend=ComputeBackend::Cpu;options.speed=EncodeSpeed::Fast;options.threads=10;options.chroma420=false;options.targetBytes=uint32_t(maxPacket);
    // Naked full-resolution wavelet, fixed quantizer, no CfL, AQ, resizing,
    // display sharpening or native reconstruction: isolate transform phase.
    WaveletRateControl rate;rate.preferredStep=80;ScopedWaveletRateControl scope(&rate);
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(10);codec->setDeblocking(0);codec->setSharpening(0);
    const auto packet=encodeWavelet(source,options,false);require(rate.actualStep==80,"Diagnostic quantizer changed.");const auto reference=codec->decode(packet,source.width,source.height);
    std::ofstream csv(output/L"results.csv");csv<<"offset,quantizer,bytes,aligned_rms,aligned_max,changed_pixels,pixels\n";
    for(int offset:{0,1,2,4,8,16,32,64}){auto shifted=source;
        for(uint32_t y=0;y<source.height;++y)for(uint32_t x=0;x<source.width;++x){const auto from=(size_t(y)*source.width+std::min(source.width-1,x+uint32_t(offset)))*4;const auto to=(size_t(y)*source.width+x)*4;std::copy_n(source.pixels.data()+from,4,shifted.pixels.data()+to);}
        const auto encoded=encodeWavelet(shifted,options,false);require(rate.actualStep==80,"Diagnostic quantizer changed.");const auto decoded=codec->decode(encoded,source.width,source.height);
        double squared=0;unsigned maximum=0;uint64_t changed=0,pixels=0;
        for(uint32_t y=320;y+320<source.height;++y)for(uint32_t x=320;x+384<source.width;++x){bool different=false;for(unsigned c=0;c<3;++c){const auto a=(size_t(y)*source.width+x)*4+c,b=(size_t(y)*source.width+x+offset)*4+c;const unsigned d=unsigned(std::abs(int(decoded.pixels[a])-int(reference.pixels[b])));squared+=d*d;maximum=std::max(maximum,d);different|=bool(d);}changed+=different;++pixels;}
        const double rms=std::sqrt(squared/(pixels*3));csv<<offset<<",80,"<<encoded.data.size()<<','<<rms<<','<<maximum<<','<<changed<<','<<pixels<<'\n';csv.flush();
        std::cout<<"offset="<<offset<<", aligned RMS="<<rms<<", maximum="<<maximum<<", changed="<<changed<<'/'<<pixels<<std::endl;
    }return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
