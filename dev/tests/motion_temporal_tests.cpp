#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/compute.hpp"
#include "fajk/hardware.hpp"
#include "fajk/entropy.hpp"
#include "../src/motion_temporal.hpp"
#include "../src/temporal_coefficients.hpp"
#include "../src/decode_internal.hpp"
#include "../src/temporal_predictive.hpp"
#include "../src/coefficient_overrides.hpp"
#include "../src/timing.hpp"
#include "../src/predictive_color_avx2.hpp"
#include "../src/motion_warp_avx2.hpp"
#include "../src/resample.hpp"
#include "../src/temporal_quality.hpp"
#include "../src/final_sharpen.hpp"
#include <iostream>
#include <source_location>
using namespace fajk;
namespace fajk {bool hasAvx2();}
namespace {
void check(bool value,std::source_location where=std::source_location::current()){if(!value)throw std::runtime_error("Motion temporal assertion at "+std::to_string(where.line()));}
template<class F>void rejected(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
void checkWarpRows(){
    if(!hasAvx2())return;
    uint32_t seed=819;auto random=[&]{seed=seed*1664525u+1013904223u;return seed;};
    for(uint32_t width:{1u,7u,8u,9u,31u,32u,33u,64u,65u,129u,513u})for(uint32_t height:{1u,2u,17u,33u})for(unsigned mode=0;mode<4;++mode){
        const auto cols=(width+31)/32,rows=(height+31)/32,maskCols=(width+7)/8;
        Bytes source(pixelBytes(width,height));for(auto& value:source)value=uint8_t(random()>>24);
        std::vector<uint32_t> vectors(size_t(cols)*rows),gradients;
        for(auto& value:vectors){const auto a=uint16_t(int(random()%1025)-512),b=uint16_t(int(random()%1025)-512);value=a|(uint32_t(b)<<16);}
        if(mode>=2){gradients.resize(vectors.size());for(auto& value:gradients)value=random();}
        Bytes modes;if(mode){modes.resize(size_t(maskCols)*((height+7)/8));for(auto& value:modes)value=mode==3?3:uint8_t(random()%4);}
        if(mode==3)std::fill(vectors.begin(),vectors.end(),0);
        const std::array<int,3> bias=mode==3?std::array<int,3>{0,0,0}:std::array<int,3>{-32,17,32};
        for(uint32_t y=0;y<height;++y){Bytes output(size_t(width)*4+32,197),expected=output;
            const auto count=motionWarpRowAvx2(source.data(),output.data()+16,width,height,y,vectors,gradients,modes,bias);check(count==width/8*8);
            for(uint32_t x=0;x<count;++x){const auto at=size_t(y/32)*cols+x/32;const auto packed=vectors[at];int dx=int(int16_t(uint16_t(packed))),dy=int(int16_t(uint16_t(packed>>16)));
                if(!gradients.empty()){const auto g=gradients[at];int values[4];for(unsigned c=0;c<4;++c)values[c]=int(int8_t(uint8_t(g>>(c*8))));
                    auto divide=[](int v){return v<0?-((-v+128)/256):(v+128)/256;};const int lx=int(x%32)-16,ly=int(y%32)-16;
                    dx+=divide(values[0]*lx+values[1]*ly);dy+=divide(values[2]*lx+values[3]*ly);}
                if(!modes.empty()&&(modes[size_t(y/8)*maskCols+x/8]&2))dx=dy=0;
                const int sx=std::clamp(int(x)*4+dx,0,int(width-1)*4),sy=std::clamp(int(y)*4+dy,0,int(height-1)*4);
                const auto ax=uint32_t(sx/4),ay=uint32_t(sy/4),bx=std::min(ax+1,width-1),by=std::min(ay+1,height-1);const int fx=sx%4,fy=sy%4;
                for(unsigned c=0;c<3;++c){auto pixel=[&](uint32_t xx,uint32_t yy){return int(source[(size_t(yy)*width+xx)*4+c]);};
                    const int value=((pixel(ax,ay)*(4-fx)+pixel(bx,ay)*fx)*(4-fy)+(pixel(ax,by)*(4-fx)+pixel(bx,by)*fx)*fy+8)/16;
                    expected[16+size_t(x)*4+c]=uint8_t(std::clamp(value+bias[c],0,255));}expected[16+size_t(x)*4+3]=255;
            }
            check(output==expected);
        }
    }
}
void checkColorRows(){
    if(!hasAvx2())return;
    uint32_t seed=317;auto random=[&]{seed=seed*1664525u+1013904223u;return seed;};
    std::array<std::array<uint8_t,64>,512> table;
    for(auto& row:table)for(auto& value:row)value=uint8_t(random()%5);
    for(uint32_t width:{1u,7u,8u,9u,15u,16u,17u,127u,128u,129u,513u})for(uint32_t phase=0;phase<8;++phase)for(unsigned trial=0;trial<4;++trial){
        const auto cw=((width+127)&~127u)/2;
        std::vector<float> yy(width);std::array<std::vector<float>,4> chroma;
        for(auto& plane:chroma){plane.resize(cw);for(auto& value:plane)value=trial?float(int(random()%16385)-8192)/32:0;}
        for(uint32_t x=0;x<width;++x){const float edges[]={.5f,std::nextafter(.5f,0.f),-.5f,std::nextafter(-.5f,0.f),1.5f,-1.5f,255.f,-255.f};yy[x]=trial?float(int(random()%65537)-32768)/64:edges[x%8];}
        std::vector<uint16_t> patterns((width+7)/8);for(auto& value:patterns)value=uint16_t(random()%512);
        Bytes pixels(size_t(width)*4+32,197);for(size_t i=16;i<pixels.size()-16;++i)pixels[i]=uint8_t(random()>>24);auto expected=pixels;
        const float fy=phase==0?0:phase%2?.25f:.75f;
        const auto count=predictiveColorRowAvx2(pixels.data()+16,width,cw,yy.data(),chroma[0].data(),chroma[1].data(),chroma[2].data(),chroma[3].data(),patterns.data(),table.data(),phase,fy);
        check(count==width/8*8);
        for(uint32_t x=0;x<count;++x){const float sx=std::max(0.f,(x+.5f)/2-.5f);const auto lo=uint32_t(sx),hi=std::min(lo+1,cw-1);const float fx=sx-lo;
            auto interpolate=[&](unsigned c){return (chroma[c][lo]*(1-fx)+chroma[c][hi]*fx)*(1-fy)+(chroma[c+1][lo]*(1-fx)+chroma[c+1][hi]*fx)*fy;};
            const float co=interpolate(0),cg=interpolate(2),delta[]={yy[x]+co-cg,yy[x]+cg,yy[x]-co-cg};
            const auto blend=table[patterns[x/8]][phase*8+x%8];
            for(unsigned c=0;c<3;++c){const float correction=std::clamp(delta[c]*blend*.25f,-255.f,255.f);const int rounded=int(double(correction)+(correction<0?-.5:.5));const auto at=16+size_t(x)*4+c;expected[at]=uint8_t(std::clamp(int(expected[at])+rounded,0,255));}
        }
        check(pixels==expected);
    }
}
Frame fixture(unsigned phase){
    Frame f{1025,769,int64_t(phase)*333333,333333,Bytes(pixelBytes(1025,769))};
    for(uint32_t y=0;y<f.height;++y)for(uint32_t x=0;x<f.width;++x){const auto at=(size_t(y)*f.width+x)*4;const int xx=int(x),yy=int(y);
        f.pixels[at]=uint8_t(110+40*std::sin(xx*.037)+20*std::cos(yy*.021));
        f.pixels[at+1]=uint8_t(120+30*std::sin((xx+yy)*.019)+phase%3);f.pixels[at+2]=uint8_t(130+35*std::cos(xx*.028)+20*std::sin(yy*.031));
        if(x>=300+phase&&x<420+phase&&y>=250&&y<370){f.pixels[at]=200;f.pixels[at+1]=50;f.pixels[at+2]=30;}f.pixels[at+3]=255;
    }return f;
}
}
int main(){try{
    checkWarpRows();
    checkColorRows();
    {
        Frame source{256,256,0,1,Bytes(pixelBytes(256,256),255)},baseline=source,candidate=source;
        for(size_t at=0;at<source.pixels.size();at+=4){for(unsigned c=0;c<3;++c){source.pixels[at+c]=100;candidate.pixels[at+c]=100;}const int noise=(at/4)&1?4:-4;baseline.pixels[at]=uint8_t(100+noise);baseline.pixels[at+1]=100;baseline.pixels[at+2]=uint8_t(100-noise);}
        check(compareTemporalQuality(source,candidate,baseline,4).accepted());
        for(uint32_t y=96;y<128;++y)for(uint32_t x=96;x<128;++x){const auto at=(size_t(y)*256+x)*4;candidate.pixels[at]=120;candidate.pixels[at+2]=80;}
        const auto local=compareTemporalQuality(source,candidate,baseline,4);
        check(local.candidate.rgb<local.independent.rgb&&local.candidate.chroma<local.independent.chroma&&local.candidate.gradient<local.independent.gradient&&local.damagedTiles&&!local.accepted());
        const auto stopped=compareTemporalQuality(source,candidate,baseline,4,nullptr,nullptr,true);
        check(!stopped.accepted()&&!stopped.measuredFullFrame&&stopped.damagedTiles);
        auto bad=baseline;for(size_t at=0;at<bad.pixels.size();at+=4){bad.pixels[at]=uint8_t(100+((at/4)&1?8:-8));bad.pixels[at+1]=100;bad.pixels[at+2]=uint8_t(100-((at/4)&1?8:-8));}
        const auto noise=compareTemporalQuality(source,bad,baseline,4,&source,&source);
        check(noise.candidate.chroma>noise.independent.chroma&&noise.candidate.gradient>noise.independent.gradient&&!noise.accepted());
    }
    // Neutral checkerboard noise has the same RGB MSE as a constant offset
    // and no opponent-color error. It must still fail the noise guard.
    {
        Frame source{65,37,0,1,Bytes(pixelBytes(65,37),255)},noisy=source,smooth=source;
        for(uint32_t y=0;y<source.height;++y)for(uint32_t x=0;x<source.width;++x)
            for(unsigned c=0;c<3;++c){const auto at=(size_t(y)*source.width+x)*4+c;
                source.pixels[at]=128;smooth.pixels[at]=132;noisy.pixels[at]=uint8_t(128+((x+y)%2?4:-4));}
        const auto result=compareTemporalQuality(source,noisy,smooth,4);
        check(result.candidate.rgb==result.independent.rgb&&result.candidate.chroma==0&&result.candidate.gradient==0);
        check(!result.accepted());
        check(compareTemporalQuality(source,smooth,noisy,4).accepted());
    }
    // Check the complete gradient against a simple per-pixel oracle, including
    // partial tiles and the left/top neighbors across 32-pixel boundaries.
    {
        Frame source{65,37,0,1,Bytes(pixelBytes(65,37),255)},a=source,b=source;
        for(size_t p=0;p<source.pixels.size()/4;++p)for(unsigned c=0;c<3;++c){
            source.pixels[p*4+c]=uint8_t((p*17+c*53)%256);
            a.pixels[p*4+c]=uint8_t((p*29+c*19+37)%256);
            b.pixels[p*4+c]=uint8_t((p*7+c*71+11)%256);
        }
        auto gradient=[&](const Frame& frame){uint64_t total=0,luma=0;
            auto opponent=[&](size_t at){const int r=int(frame.pixels[at])-source.pixels[at];
                const int g=int(frame.pixels[at+1])-source.pixels[at+1];const int blue=int(frame.pixels[at+2])-source.pixels[at+2];
                return std::array<int,3>{r-blue,2*g-r-blue,r+2*g+blue};};
            for(uint32_t y=0;y<source.height;++y)for(uint32_t x=0;x<source.width;++x){const auto at=(size_t(y)*source.width+x)*4;const auto [co,cg,yy]=opponent(at);
                for(auto offset:{x?size_t(4):size_t(0),y?size_t(source.width)*4:size_t(0)})if(offset){const auto [nco,ncg,ny]=opponent(at-offset);const int64_t dc=co-nco,dg=cg-ncg,dy=yy-ny;total+=4*dc*dc+dg*dg;luma+=dy*dy;}}
            return std::pair{total,luma};};
        for(uint32_t workers:{1u,4u}){const auto result=compareTemporalQuality(source,a,b,workers);
            check(result.candidate.gradient==gradient(a).first&&result.independent.gradient==gradient(b).first);
            check(result.candidate.lumaGradient==gradient(a).second&&result.independent.lumaGradient==gradient(b).second);}
    }
    // The encoder reuses selected quantized coefficients and its first warp.
    // Compare that reconstruction to the independent wire decoder, including
    // quarter-pixel motion, stationary overrides and non-even image borders.
    const auto base=fixture(0),changed=fixture(1);
    {EncodeOptions invalid;invalid.reconstruction441.protectText=true;
        rejected([&]{MotionTemporalEncoder encoder(invalid,120,6,10);});}
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu}){
        EncodeOptions options;options.backend=backend;options.speed=EncodeSpeed::Fast;options.targetBytes=80000;options.threads=4;
        MotionTemporalEncoder resized(options,0,16,10);
        auto first=resized.encode(base,0);
        check(bool(readMotionRoot(first,base.width,base.height,4)));
        auto smaller=resamplePhoto(base,513,385,4,{},2);smaller.pts=base.duration;
        const auto independent=resized.encode(smaller,1);check(independent.referenceIndex==UINT64_MAX&&!independent.reference);
        check(bool(readMotionRoot(independent,smaller.width,smaller.height,4)));
        smaller.pts+=smaller.duration;const auto hold=resized.encode(smaller,2);check(hold.data.size()==8&&hold.referenceIndex==1);
    }
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu}){
        EncodeOptions automatic;automatic.backend=backend;automatic.speed=EncodeSpeed::Fast;automatic.targetBytes=24000;
        auto explicitWorkers=automatic;explicitWorkers.threads=safeWorkers(base.width,base.height,false,0);
        MotionTemporalEncoder a(automatic,0,6,10),b(explicitWorkers,0,6,10);
        for(unsigned n=0;n<2;++n){auto source=fixture(n);auto left=a.encode(source,n),right=b.encode(std::move(source),n);
            check(left.data==right.data&&left.referenceIndex==right.referenceIndex);}
    }
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu})for(auto speed:{EncodeSpeed::Fast,EncodeSpeed::MoreQuality,EncodeSpeed::MaxCompression})for(uint32_t budget:{24000u,80000u}){
        EncodeOptions options;options.backend=backend;options.speed=speed;options.threads=4;options.targetBytes=budget;
        options.adaptiveQuantizationEnabled=budget==80000;
        auto codec=makeCodec(CodecId::Fajk);CoefficientEncodeCapture capture;
        ScopedCoefficientEncodeCapture scope(&capture);auto packet=codec->encode(base,options);
        auto reused=readMotionRoot(packet,base.width,base.height,4);
        if(!capture.used)std::cerr<<"Capture backend="<<int(backend)<<" speed="<<int(speed)<<" valid="<<capture.valid<<" step="<<capture.fixedStep<<" payload="<<capture.nativePacket.size()<<" dimensions="<<capture.width<<'x'<<capture.height<<'\n';
        check(bool(reused)&&(!capture.valid||capture.used));
        {ScopedCoefficientEncodeCapture none(nullptr);auto ordinary=readMotionRoot(packet,base.width,base.height,4);
            check(bool(ordinary)&&ordinary->image.pixels==reused->image.pixels);}
        if(!capture.nativePacket.empty())capture.nativePacket[0]^=1;capture.used=false;
        auto fallback=readMotionRoot(packet,base.width,base.height,4);check(bool(fallback)&&!capture.used&&fallback->image.pixels==reused->image.pixels);
        if(backend==ComputeBackend::Cpu){
            // The canonical root already owns the CPU inverse. Its regular
            // display reconstruction must exactly match the ordinary decoder
            // before the encoder can share it instead of doing another inverse.
            codec->setThreads(4);codec->setComputeBackend(ComputeBackend::Cpu);
            const auto ordinary=codec->decode(packet,base.width,base.height);
            Frame shared;const auto rootWithDisplay=readMotionRoot(packet,base.width,base.height,4,&shared);
            check(bool(rootWithDisplay));
            check(ordinary.pixels==shared.pixels);
        }
        // A matching capture must also preserve every coefficient-recoding
        // decision, not merely the reconstructed key's rounded RGB pixels.
        {ScopedCoefficientEncodeCapture none(nullptr);capture={};}
        packet=codec->encode(base,options);
        Packet referencePacket;{ScopedCoefficientEncodeCapture none(nullptr);referencePacket=codec->encode(changed,options);}
        const auto capturedDelta=encodeCoefficientTemporal(packet,referencePacket,4);
        if(capturedDelta&&capturedDelta->packet.data.size()>8&&capture.valid)check(capture.used);
        {ScopedCoefficientEncodeCapture none(nullptr);const auto ordinaryDelta=encodeCoefficientTemporal(packet,referencePacket,4);
            check(bool(capturedDelta)==bool(ordinaryDelta));if(capturedDelta)check(capturedDelta->packet.data==ordinaryDelta->packet.data);}
    }
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu}){
        auto shifted=base;const std::array<int,3> shift{-3,5,2};
        for(size_t at=0;at<shifted.pixels.size();at+=4)for(unsigned c=0;c<3;++c)shifted.pixels[at+c]=uint8_t(std::clamp(int(shifted.pixels[at+c])+shift[c],0,255));
        auto corrected=trialPredictive(shifted,base,base,4,6,1,backend,{},10,6,false,65,nullptr,false,true,true);
        auto previous=trialPredictive(shifted,base,base,4,6,1,backend,{},10,6,false,65,nullptr,false,true,true,SIZE_MAX,true,false);
        check(corrected.image.pixels==shifted.pixels&&corrected.packet.data.size()<previous.packet.data.size());
        ScopedComputeBackend compute(backend);check(decodePredictiveTrial(corrected.packet,base,4).pixels==corrected.image.pixels);
        std::cout<<"Uniform illumination residual "<<previous.packet.data.size()<<" -> "<<corrected.packet.data.size()<<" bytes, exact reconstruction\n";
    }
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu})for(uint32_t step:{1u,18u,64u,256u}){
        auto trial=trialPredictive(changed,base,base,4,6,step,backend,{},10,6,false,65,nullptr,false,true,true);
        auto originalMotion=trialPredictive(changed,base,base,4,6,step,backend,{},10,6,false,65,nullptr,false,true,true,SIZE_MAX,true,true,false);
        check(trial.packet.data.size()<=originalMotion.packet.data.size()&&trial.image.pixels==originalMotion.image.pixels);
        check(trial.decisionBytes==originalMotion.packet.data.size());
        {ScopedComputeBackend compute(backend);check(decodePredictiveTrial(originalMotion.packet,base,4).pixels==trial.image.pixels);}
        if(backend==ComputeBackend::Gpu){
            auto separate=trialPredictive(changed,base,base,4,6,step,backend,{},10,6,false,65,nullptr,false,true,true,SIZE_MAX,false);
            check(trial.packet.data==separate.packet.data&&trial.image.pixels==separate.image.pixels&&trial.ages==separate.ages);
        }
        ScopedComputeBackend compute(backend);
        check(trial.image.pixels==decodePredictiveTrial(trial.packet,base,4).pixels);
        if(step==18){auto rejectedTrial=trialPredictive(changed,base,base,4,6,step,backend,{},10,6,false,65,nullptr,false,true,true,0);
            check(rejectedTrial.overBudget&&rejectedTrial.packet.data.empty()&&rejectedTrial.image.pixels.empty());
            auto noRetry=trialPredictive(changed,base,base,4,6,step,backend,{},10,6,false,65,nullptr,false,true,true,trial.decisionBytes,true,true,true,36);
            check(!noRetry.overBudget&&noRetry.rateRetries==0&&noRetry.selectedStep==step);
            check(noRetry.packet.data==trial.packet.data&&noRetry.image.pixels==trial.image.pixels);
            auto exactCap=trialPredictive(changed,base,base,4,6,step,backend,{},10,6,false,65,nullptr,false,true,true,trial.decisionBytes);
            check(!exactCap.overBudget&&exactCap.packet.data==trial.packet.data&&exactCap.image.pixels==trial.image.pixels);
            auto shortCap=trialPredictive(changed,base,base,4,6,step,backend,{},10,6,false,65,nullptr,false,true,true,trial.decisionBytes-1);
            check(shortCap.overBudget&&shortCap.packet.data.empty()&&shortCap.image.pixels.empty());
            const auto beforeCoarse=gpuActivity();
            auto coarse=trialPredictive(changed,base,base,4,6,36,backend,{},10,6,false,65,nullptr,false,true,true);
            const auto afterCoarse=gpuActivity();
            check(coarse.decisionBytes<trial.decisionBytes);
            const auto limit=(trial.decisionBytes+coarse.decisionBytes)/2;
            const auto beforeRetry=gpuActivity();
            auto retry=trialPredictive(changed,base,base,4,6,18,backend,{},10,6,false,65,nullptr,false,true,true,limit,true,true,true,36);
            const auto afterRetry=gpuActivity();
            check(!retry.overBudget&&retry.selectedStep==36&&retry.rateRetries==1&&retry.decisionBytes<=limit);
            check(retry.packet.data==coarse.packet.data&&retry.image.pixels==coarse.image.pixels&&retry.ages==coarse.ages);
            check(decodePredictiveTrial(retry.packet,base,4).pixels==retry.image.pixels);
            if(backend==ComputeBackend::Gpu){
                check(afterRetry.transforms-beforeRetry.transforms==afterCoarse.transforms-beforeCoarse.transforms);
                check(afterRetry.analyses-beforeRetry.analyses==afterCoarse.analyses-beforeCoarse.analyses);
                check(afterRetry.uploadedBytes-beforeRetry.uploadedBytes==afterCoarse.uploadedBytes-beforeCoarse.uploadedBytes);
                check(afterRetry.downloadedBytes-beforeRetry.downloadedBytes==afterCoarse.downloadedBytes-beforeCoarse.downloadedBytes);
            }
            auto impossible=trialPredictive(changed,base,base,4,6,18,backend,{},10,6,false,65,nullptr,false,true,true,0,true,true,true,36);
            check(impossible.overBudget&&impossible.rateRetries==1&&impossible.packet.data.empty()&&impossible.image.pixels.empty());
            auto cut=changed;for(size_t at=0;at<cut.pixels.size();at+=4){cut.pixels[at]=0;cut.pixels[at+1]=255;cut.pixels[at+2]=0;}
            auto cutTrial=trialPredictive(cut,base,base,4,6,18,backend,{},10,6,false,65,nullptr,false,true,true,0,true,true,true,36);
            check(cutTrial.predictionMse>1024&&cutTrial.overBudget&&cutTrial.rateRetries==0&&cutTrial.packet.data.empty());
            rejected([&]{trialPredictive(changed,base,base,4,6,18,backend,{},10,6,false,65,nullptr,false,true,true,SIZE_MAX,true,true,true,17);});
            rejected([&]{trialPredictive(changed,base,base,4,6,18,backend,{},10,6,false,65,nullptr,false,true,true,SIZE_MAX,true,true,true,257);});
        }
    }
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu})for(bool adjacent:{false,true}){
        auto filtered=trialPredictive(changed,base,base,4,6,36,backend,{},10,6,false,65,nullptr,false,true,true,SIZE_MAX,true,true,true,72,true,adjacent);
        auto separate=trialPredictive(changed,base,base,4,6,36,backend,{},10,6,false,65,nullptr,false,true,true,SIZE_MAX,false,true,true,72,true,adjacent);
        check(filtered.packet.data[6]==(adjacent?16:15)&&filtered.packet.data==separate.packet.data&&filtered.image.pixels==separate.image.pixels);
        auto unfiltered=trialPredictive(changed,base,base,4,6,36,backend,{},10,6,false,65,nullptr,false,true,true,SIZE_MAX,true,true,true,72,false);
        check(unfiltered.packet.data[6]==14&&filtered.image.pixels!=unfiltered.image.pixels);
        ScopedComputeBackend compute(backend);std::vector<TimingEvent> filteredEvents;const auto beforeFiltered=gpuActivity();
        {TimingCapture capture(&filteredEvents);check(decodePredictiveTrial(filtered.packet,base,4).pixels==filtered.image.pixels);}
        const auto inverseCount=std::count_if(filteredEvents.begin(),filteredEvents.end(),[](const auto& e){return std::string_view(e.stage)=="decode.motion.inverse";});
        check(inverseCount==(gpuActivity().transforms>beforeFiltered.transforms?0:1));
        auto tint=base;for(size_t i=0;i<tint.pixels.size();i+=4)for(unsigned c=0;c<3;++c)tint.pixels[i+c]=uint8_t(std::min(255,int(tint.pixels[i+c])+3));
        auto exact=trialPredictive(base,base,tint,4,6,36,backend,{},10,6,false,65,nullptr,false,true,true,SIZE_MAX,true,true,true,72,true,adjacent);
        check(exact.image.pixels==tint.pixels);
        const auto beforeExact=gpuActivity();std::vector<TimingEvent> exactEvents;
        {TimingCapture capture(&exactEvents);check(decodePredictiveTrial(exact.packet,tint,4).pixels==tint.pixels);}
        check(gpuActivity().referenceFilters==beforeExact.referenceFilters);
        check(std::none_of(exactEvents.begin(),exactEvents.end(),[](const auto& event){return std::string_view(event.stage)=="decode.motion.inverse";}));
        // Skipping synthesis must not skip validation of coded residuals.
        auto damaged=exact.packet;damaged.data.pop_back();
        rejected([&]{decodePredictiveTrial(damaged,tint,4);});
        auto finest=trialPredictive(changed,base,base,4,0,1,backend,{},10,0,false,65,nullptr,false,true,true,SIZE_MAX,true,true,true,0,true,adjacent);
        auto withoutFilter=finest.packet;withoutFilter.data[6]=14;
        check(decodePredictiveTrial(withoutFilter,base,4).pixels==finest.image.pixels);
    }
    // New picture content must correct old coding error instead of integrating
    // it indefinitely. Use flat planes so quantization does not hide the DC
    // response; disable illumination fitting to exercise the residual path.
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu}){
        Frame old{512,512,0,1,Bytes(pixelBytes(512,512),255)},decoded=old,current=old;
        for(size_t at=0;at<old.pixels.size();at+=4)for(unsigned c=0;c<3;++c){old.pixels[at+c]=100;decoded.pixels[at+c]=120;current.pixels[at+c]=120;}
        for(unsigned n=0;n<5;++n){
            auto trial=trialPredictive(current,old,decoded,4,6,1,backend,{},10,32,false,65,nullptr,false,true,true,SIZE_MAX,true,false,true,0,true);
            ScopedComputeBackend compute(backend);check(decodePredictiveTrial(trial.packet,decoded,4).pixels==trial.image.pixels);
            const int expectedError=10>>n;
            for(size_t at=0;at<trial.image.pixels.size();at+=4)for(unsigned c=0;c<3;++c)check(std::abs(int(trial.image.pixels[at+c])-int(current.pixels[at+c]))<=expectedError+1);
            decoded=std::move(trial.image);old=current;
            // Keep a real source change, rather than the exact-source skip.
            for(size_t at=0;at<current.pixels.size();at+=4)for(unsigned c=0;c<3;++c)++current.pixels[at+c];
        }
    }
    // 4:2:0 residuals cannot cancel a one-pixel opponent-color checkerboard:
    // its cell averages are zero. Stride-two filtering sampled only the same
    // phase and left this inherited reference noise completely untouched.
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu}){
        Frame old{512,512,0,1,Bytes(pixelBytes(512,512),255)},current=old,decoded=old;
        for(uint32_t y=0;y<512;++y)for(uint32_t x=0;x<512;++x){const auto at=(size_t(y)*512+x)*4;const int noise=(x+y)&1?16:-16;
            for(unsigned c=0;c<3;++c){old.pixels[at+c]=99;current.pixels[at+c]=100;decoded.pixels[at+c]=uint8_t(100+(c==0?noise:c==2?-noise:0));}}
        auto legacy=trialPredictive(current,old,decoded,4,6,32,backend,{},10,8,false,65,nullptr,false,true,true,SIZE_MAX,true,false,true,0,true);
        auto adjacent=trialPredictive(current,old,decoded,4,6,32,backend,{},10,8,false,65,nullptr,false,true,true,SIZE_MAX,true,false,true,0,true,true);
        uint64_t before=0,after=0;
        for(size_t at=0;at<current.pixels.size();at+=4){const int a=int(legacy.image.pixels[at])-legacy.image.pixels[at+2],b=int(adjacent.image.pixels[at])-adjacent.image.pixels[at+2];before+=a*a;after+=b*b;}
        check(after*2<before);
        for(auto decodeBackend:{ComputeBackend::Cpu,ComputeBackend::Gpu}){ScopedComputeBackend compute(decodeBackend);
            check(decodePredictiveTrial(adjacent.packet,decoded,4).pixels==adjacent.image.pixels);}
        std::cout<<"Checkerboard chroma error "<<before<<" -> "<<after<<"\n";
    }
    EncodeOptions options;options.speed=EncodeSpeed::Fast;options.threads=4;options.targetBytes=24000;
    options.backend=gpuInfo().available?ComputeBackend::Gpu:ComputeBackend::Cpu;
    MotionTemporalEncoder encoder(options,0,6,10);std::vector<Packet> packets;std::optional<MotionReference> repairedReference;
    const auto path=std::filesystem::temp_directory_path()/L"fajk-motion-temporal.fajkv";
    SequenceInfo info{1025,769,30,1,CodecId::Fajk,1,0,0,true,true};
    uint32_t predicted=0;
    {SequenceWriter writer(path,info,true);for(unsigned n=0;n<20;++n){auto source=fixture(n);auto packet=encoder.encode(source,n);check(packet.data.size()<=options.targetBytes);
        if(packet.data[4]==motionTemporalVersion&&packet.data[5]==1){check(packet.data.size()>32&&(packet.data[18]==15||packet.data[18]==16));ByteReader residualStep{std::span(packet.data).subspan(28,4)};check(residualStep.u32()<=128);}
        predicted+=packet.referenceIndex!=UINT64_MAX;writer.append(packet);packets.push_back(std::move(packet));}
        auto source=fixture(19);source.pts=20*333333ll;source.duration=60*10000000ll;
        auto hold=encoder.encode(source,20);check(hold.data.size()==8&&hold.referenceIndex==19);writer.append(hold);packets.push_back(hold);
        source=fixture(20);source.pts=packets.back().pts+packets.back().duration;auto refresh=encoder.encode(source,21);check(refresh.referenceIndex==UINT64_MAX);writer.append(refresh);packets.push_back(refresh);
        source=fixture(21);source.pts=packets.back().pts+packets.back().duration;source.pixels[3]=100;
        auto alpha=encoder.encode(source,22);check(alpha.referenceIndex==UINT64_MAX);writer.append(alpha);packets.push_back(alpha);
        source=fixture(22);source.pts=packets.back().pts+packets.back().duration;
        auto opaque=encoder.encode(source,23);check(opaque.referenceIndex==UINT64_MAX);writer.append(opaque);packets.push_back(opaque);
        // A predictive frame can follow an exact coefficient fallback. Keep a
        // real wire-level P sample for malformed/seek tests independently of
        // the encoder's quality-driven decision on this particular fixture.
        auto current=source;current.pts+=current.duration;for(size_t at=0;at<current.pixels.size();at+=4)for(unsigned c=0;c<3;++c)current.pixels[at+c]=uint8_t(std::min(255,int(current.pixels[at+c])+1));
        auto independent=makeCodec(CodecId::Fajk)->encode(current,options);
        auto exact=encodeCoefficientTemporal(independent,opaque,4);check(bool(exact));exact->packet.referenceIndex=23;writer.append(exact->packet);packets.push_back(exact->packet);
        auto root=readMotionRoot(independent,source.width,source.height,4);check(bool(root));
        auto witness=resamplePhoto(current,root->image.width,root->image.height,4,{},2);auto next=current;next.pts+=next.duration;next.pixels[0]^=3;
        auto working=resamplePhoto(next,root->image.width,root->image.height,4,{},2);
        auto trial=trialPredictive(working,witness,root->image,4,6,1,options.backend,{},10,0,false,65,nullptr,false,true,true,SIZE_MAX,true,true,true,0,true,true);
        ByteWriter out;out.u32(0x334b4a46);out.u8(motionTemporalVersion);out.u8(1);out.u16(0);out.u32(uint32_t(trial.packet.data.size()));out.bytes(trial.packet.data);
        Packet mixed{std::move(out.data),next.pts,next.duration};mixed.referenceIndex=24;writer.append(mixed);packets.push_back(mixed);
        ByteWriter repeat;repeat.u32(0x334b4a46);repeat.u8(motionTemporalVersion);repeat.u8(2);repeat.u16(0);
        Packet held{std::move(repeat.data),next.pts+next.duration,next.duration};held.referenceIndex=25;writer.append(held);packets.push_back(held);
        auto patched=trial.image;const auto columns=(patched.width+31)/32,rows=(patched.height+31)/32;
        const std::array<uint32_t,2> tiles{0,columns*rows-1};
        for(auto tile:tiles){const auto x0=tile%columns*32,y0=tile/columns*32;
            for(uint32_t y=y0;y<std::min(patched.height,y0+32);++y)for(uint32_t x=x0;x<std::min(patched.width,x0+32);++x){const auto at=(size_t(y)*patched.width+x)*4;patched.pixels[at]^=7;patched.pixels[at+2]^=11;}}
        const auto repairs=encodeMotionRepairs(trial.image,patched,tiles,4);
        ByteWriter repaired;repaired.u32(0x334b4a46);repaired.u8(motionTemporalVersion);repaired.u8(3);repaired.u16(0);repaired.u32(uint32_t(trial.packet.data.size()));repaired.bytes(trial.packet.data);repaired.bytes(repairs);
        Packet patchPacket{std::move(repaired.data),held.pts+held.duration,held.duration};patchPacket.referenceIndex=24;writer.append(patchPacket);packets.push_back(patchPacket);
        repairedReference=*root;repairedReference->image=patched;
        held.pts=patchPacket.pts+patchPacket.duration;held.referenceIndex=27;writer.append(held);packets.push_back(held);
        MotionResidualSettings reuseSettings;reuseSettings.step=1;reuseSettings.tolerance=0;reuseSettings.backend=options.backend;
        auto reused=trialMotion(working,working,patched,4,reuseSettings);
        ByteWriter subsequent;subsequent.u32(0x334b4a46);subsequent.u8(motionTemporalVersion);subsequent.u8(1);subsequent.u16(0);subsequent.u32(uint32_t(reused.packet.data.size()));subsequent.bytes(reused.packet.data);
        Packet following{std::move(subsequent.data),held.pts+held.duration,held.duration};following.referenceIndex=27;writer.append(following);packets.push_back(following);writer.finish();
    }
    check(predicted>=5);
    {
        auto validation=makeCodec(CodecId::Fajk);validation->setThreads(4);validation->setComputeBackend(options.backend);
        SequenceReader reader(path);Frame previousSource,previousOutput;
        for(uint64_t n=0;n<20;++n){auto source=fixture(unsigned(n));const auto chosen=validation->decode(reader.read(n),info.width,info.height);
            const auto baseline=validation->decode(validation->encode(source,options),info.width,info.height);
            check(compareTemporalQuality(source,chosen,baseline,4,n?&previousSource:nullptr,n?&previousOutput:nullptr).accepted());
            previousSource=std::move(source);previousOutput=chosen;
        }
    }
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(4);
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu})for(uint32_t filter:{0u,100u}){
        codec->setComputeBackend(backend);codec->setDeblocking(filter);codec->setSharpening(50);
        SequenceReader forward(path);std::vector<Bytes> images;
        for(uint64_t n=0;n<packets.size();++n){const auto packet=forward.read(n);uint32_t depth=0;for(auto parent=packet.reference;parent;parent=parent->reference)++depth;check(depth<=coefficientSeekDepth);auto image=codec->decode(packet,info.width,info.height);check(image.pts==packet.pts&&image.duration==packet.duration);images.push_back(std::move(image.pixels));}
        check(images[19]==images[20]&&images[25]==images[26]&&images[27]==images[28]&&images[27]==images[29]);
        {ScopedComputeBackend compute(backend);auto expected=renderMotionReference(*repairedReference,packets[27].pts,packets[27].duration,info.width,info.height,4,filter);
            finalSharpen(expected.pixels,info.width,info.height,4,50);check(images[27]==expected.pixels);}
        {SequenceReader sequential(path);const auto expected=codec->decode(sequential.read(25),info.width,info.height);std::vector<TimingEvent> events;
            {TimingCapture capture(&events);check(codec->decode(sequential.read(26),info.width,info.height).pixels==expected.pixels);}
            check(std::none_of(events.begin(),events.end(),[](const auto& e){return std::string_view(e.stage).starts_with("decode.motion.")||std::string_view(e.stage).starts_with("decode.coefficients.");}));}
        // A mutable copy must never seed the motion cache under another
        // immutable packet's identity, even when both payloads are valid.
        {
            SequenceReader altered(path);
            auto original=altered.read(27);auto different=altered.read(25);
            different.keyIdentity=original.keyIdentity;
            codec->decode(different,info.width,info.height);
            auto following=altered.read(29);following.reference=original.keyIdentity;
            check(codec->decode(following,info.width,info.height).pixels==images[29]);
        }
        SequenceReader random(path);
        for(uint64_t n:{29u,28u,27u,26u,25u,24u,20u,1u,19u,16u,0u,12u,21u,20u}){const auto packet=random.read(n);const auto image=codec->decode(packet,info.width,info.height);check(image.pixels==images[n]);
            Bytes guarded(pixelBytes(info.width,info.height)+32,197);decodeFrameToBuffer(packet,info.width,info.height,4,filter,std::span(guarded).subspan(16,image.pixels.size()),50,false,backend);
            check(std::equal(image.pixels.begin(),image.pixels.end(),guarded.begin()+16)&&std::all_of(guarded.begin(),guarded.begin()+16,[](auto b){return b==197;})&&std::all_of(guarded.end()-16,guarded.end(),[](auto b){return b==197;}));}
    }
    {SequenceReader reader(path);Packet sample;uint64_t sampleIndex=0;
    for(uint64_t n=1;n<packets.size();++n){auto p=reader.read(n);if(p.data[4]==motionTemporalVersion&&p.data[5]==1){sample=std::move(p);sampleIndex=n;break;}}
    check(!sample.data.empty());
    const auto expected=codec->decode(sample,info.width,info.height);
    {SequenceReader fresh(path);auto same=fresh.read(sampleIndex);std::vector<TimingEvent> events;
        {TimingCapture capture(&events);check(codec->decode(same,info.width,info.height).pixels==expected.pixels);}
        check(std::none_of(events.begin(),events.end(),[](const auto& e){return std::string_view(e.stage)=="decode.motion.root";}));}
    auto poison=[](auto&& self,std::shared_ptr<const Packet> packet)->std::shared_ptr<const Packet>{
        auto copy=std::make_shared<Packet>(*packet);copy->keyIdentity.reset();
        if(copy->reference)copy->reference=self(self,copy->reference);else copy->data[0]^=255;return copy;
    };
    auto stale=sample;stale.keyIdentity.reset();stale.reference=poison(poison,sample.reference);
    rejected([&]{codec->decode(stale,info.width,info.height);});
    check(codec->decode(sample,info.width,info.height).pixels==expected.pixels);
    for(auto length:{0u,4u,7u,8u,11u,20u}){auto bad=sample;bad.data.resize(length);rejected([&]{codec->decode(bad,info.width,info.height);});}
    for(auto offset:{0u,4u,5u,6u,8u,12u,16u,18u}){auto bad=sample;bad.data[offset]^=255;rejected([&]{codec->decode(bad,info.width,info.height);});}
    auto trailing=sample;trailing.data.push_back(0);rejected([&]{codec->decode(trailing,info.width,info.height);});
    auto missing=sample;missing.reference.reset();rejected([&]{codec->decode(missing,info.width,info.height);});}
    {SequenceReader reader(path);const auto sample=reader.read(27);ByteReader header{sample.data};header.bytes(8);header.bytes(header.u32());const auto offset=header.pos;
        auto truncated=sample;truncated.data.resize(offset);rejected([&]{codec->decode(truncated,info.width,info.height);});
        auto empty=sample;empty.data[offset]=0;rejected([&]{codec->decode(empty,info.width,info.height);});
        ByteWriter duplicate;duplicate.bytes(std::span(sample.data).first(offset));duplicate.var(2);duplicate.var(0);duplicate.var(0);duplicate.var(0);duplicate.var(0);
        auto bad=sample;bad.data=std::move(duplicate.data);rejected([&]{codec->decode(bad,info.width,info.height);});
        check(codec->decode(sample,info.width,info.height).pixels==codec->decode(reader.read(28),info.width,info.height).pixels);
    }
    std::filesystem::remove(path);
    std::cout<<"Max compression: motion/residual/local repairs, CPU/GPU presentation, random seek, exact holds, five-second refresh, display filters, guarded buffers, malformed packets PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
