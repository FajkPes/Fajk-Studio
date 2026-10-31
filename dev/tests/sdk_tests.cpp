#include "fajk/decode.h"
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "../src/temporal.hpp"
#include "../src/temporal_coefficients.hpp"
#include "../src/motion_temporal.hpp"
#include "../src/temporal_predictive.hpp"
#include "../src/resample.hpp"
#include "fajk/entropy.hpp"
#include <iostream>
#include <memory>
#include <source_location>
#include <windows.h>

using namespace fajk;
namespace {
void check(bool value,std::source_location at=std::source_location::current()){
    if(!value)throw std::runtime_error("SDK assertion at "+std::to_string(at.line()));
}
struct Fixture {
    Path path;
    explicit Fixture(bool alpha,bool coefficients=false,bool motion=false,bool repairs=false){
        path=std::filesystem::temp_directory_path()/(L"fajk-sdk-"+std::to_wstring(GetCurrentProcessId())+(motion?L"-motion.fajkv":coefficients?L"-coefficients.fajkv":alpha?L"-alpha.fajkv":L"-correction.fajkv"));
        Frame source{259,197,0,333333,Bytes(pixelBytes(259,197))};
        for(uint32_t y=0;y<source.height;++y)for(uint32_t x=0;x<source.width;++x){const size_t at=(size_t(y)*source.width+x)*4;
            source.pixels[at]=uint8_t(x+y);source.pixels[at+1]=uint8_t(x*3+y/2);source.pixels[at+2]=uint8_t(x/3+y*5);source.pixels[at+3]=alpha?uint8_t(x+y):255;}
        EncodeOptions options;options.speed=EncodeSpeed::MaxCompression;options.targetBytes=12000;options.threads=2;
        auto codec=makeCodec(CodecId::Fajk);TemporalEncoder temporal(options,120,alpha?0:3,10);
        CoefficientTemporalEncoder recoder(2,120,true);
        MotionTemporalEncoder motionEncoder(options,120,6,10);Frame previous;std::optional<MotionReference> motionRoot;Frame motionWitness;
        SequenceInfo info{source.width,source.height,30,1,CodecId::Fajk,1,0,0,true,coefficients||motion};SequenceWriter writer(path,info,true);
        for(uint64_t n=0;n<3;++n){auto frame=source;frame.pts=int64_t(n)*frame.duration;
            if(n==1&&!coefficients){
                if(!alpha)for(size_t p=0;p<frame.pixels.size();p+=4)for(size_t c=0;c<3;++c)frame.pixels[p+c]=uint8_t(std::min(255,int(frame.pixels[p+c])+2));
                for(uint32_t y=192;y<197;++y)for(uint32_t x=256;x<259;++x)frame.pixels[(size_t(y)*259+x)*4]^=127;
                if(alpha)frame.pixels.back()^=1;
            }else if(n==2){if(motion){frame=previous;frame.pts=int64_t(n)*frame.duration;}else for(size_t p=0;p<frame.pixels.size();p+=4)for(size_t c=0;c<3;++c)frame.pixels[p+c]^=255;}
            auto packet=motion?std::optional<Packet>(motionEncoder.encode(frame,n)):coefficients?std::optional<Packet>(recoder.encode(codec->encode(frame,options),n)):temporal.encode(frame,n);if(!packet){packet=codec->encode(frame,options);temporal.commitKey(*packet,n);}
            // Exercise the public SDK's predictive wire path even when the
            // quality selector correctly rejects motion on this tiny fixture.
            if(motion&&n==0){motionRoot=readMotionRoot(*packet,source.width,source.height,2);check(bool(motionRoot));motionWitness=resamplePhoto(frame,motionRoot->image.width,motionRoot->image.height,2,{},2);}
            if(motion&&n==1){auto working=resamplePhoto(frame,motionRoot->image.width,motionRoot->image.height,2,{},2);
                auto trial=trialPredictive(working,motionWitness,motionRoot->image,2,6,1,ComputeBackend::Cpu,{},10,0,false,65,nullptr,false,true,true,SIZE_MAX,true,true,true,0,true,true);
                Bytes patch;
                if(repairs){auto target=trial.image;target.pixels[target.pixels.size()-4]^=31;
                    const auto columns=(target.width+31)/32,rows=(target.height+31)/32;const std::array<uint32_t,1> tiles{columns*rows-1};
                    patch=encodeMotionRepairs(trial.image,target,tiles,2);}
                ByteWriter out;out.u32(0x334b4a46);out.u8(motionTemporalVersion);out.u8(repairs?3:1);out.u16(0);out.u32(uint32_t(trial.packet.data.size()));out.bytes(trial.packet.data);out.bytes(patch);
                packet=Packet{std::move(out.data),frame.pts,frame.duration};packet->referenceIndex=0;
            }
            if(motion&&n==2){ByteWriter out;out.u32(0x334b4a46);out.u8(motionTemporalVersion);out.u8(2);out.u16(0);packet=Packet{std::move(out.data),frame.pts,frame.duration};packet->referenceIndex=1;}
            if(n==1)check(packet->data[4]==(motion?motionTemporalVersion:coefficients?coefficientTemporalVersion:alpha?temporalPacketVersion:temporalCorrectionVersion));
            if(motion&&n==2)check(packet->data.size()==8&&packet->data[5]==2);
            previous=std::move(frame);
            writer.append(*packet);
        }
        writer.finish();
    }
    ~Fixture(){std::error_code error;std::filesystem::remove(path,error);}
};
}
int main(){try{
    check(fajk_decode_abi_version()==1);uint64_t checked=0;
    for(unsigned mode=0;mode<5;++mode){
        Fixture fixture(mode==1,mode==2,mode>=3,mode==4);SequenceReader reader(fixture.path);auto codec=makeCodec(CodecId::Fajk);
        for(uint32_t workers:{1,2,10})for(uint32_t backend:{0,1}){
            char error[256];const auto utf8=fixture.path.u8string();
            std::unique_ptr<fajk_decoder,decltype(&fajk_close)> decoder(fajk_open(reinterpret_cast<const char*>(utf8.c_str()),error,sizeof(error)),fajk_close);
            check(bool(decoder));fajk_sequence_info info{};check(fajk_get_info(decoder.get(),&info)==-1);
            info.struct_size=sizeof(info);check(fajk_get_info(decoder.get(),&info)==0&&info.width==259&&info.height==197&&info.frame_count==3);
            check(fajk_set_threads(decoder.get(),workers)==0&&fajk_set_backend(decoder.get(),backend)==0);
            check(fajk_set_sharpening(decoder.get(),200)==0&&fajk_set_sharpening(decoder.get(),201)==-1);
            codec->setThreads(workers);codec->setComputeBackend(ComputeBackend(backend));
            for(uint32_t padding:{0,1,28})for(uint32_t setting:{0,1,2,3}){
                const uint32_t deblocking=setting==1?0:100,sharpening=setting==3?200:setting*50;
                check(fajk_set_deblocking(decoder.get(),deblocking)==0&&fajk_set_sharpening(decoder.get(),sharpening)==0);
                codec->setDeblocking(deblocking);codec->setSharpening(sharpening);
                const auto row=size_t(info.width)*4,stride=row+padding,capacity=stride*(info.height-1)+row;
                Bytes output(capacity+32,197);
                check(fajk_decode_frame(decoder.get(),0,output.data()+16,capacity-1,stride,nullptr,nullptr)==-1);
                check(fajk_decode_frame(decoder.get(),3,output.data()+16,capacity,stride,nullptr,nullptr)==-1);
                for(uint64_t index:{1,2,0,1,0,2}){
                    const auto packet=reader.read(index);const auto expected=codec->decode(packet,info.width,info.height);
                    int64_t pts=-1,duration=-1;
                    check(fajk_decode_frame(decoder.get(),index,output.data()+16,capacity,stride,&pts,&duration)==0);
                    check(pts==packet.pts&&duration==packet.duration&&!*fajk_last_error(decoder.get()));
                    for(size_t p=0;p<16;++p)check(output[p]==197&&output[output.size()-1-p]==197);
                    for(uint32_t y=0;y<info.height;++y){const auto at=16+size_t(y)*stride;
                        check(std::equal(expected.pixels.begin()+size_t(y)*row,expected.pixels.begin()+size_t(y+1)*row,output.begin()+at));
                        if(y+1<info.height)for(size_t p=row;p<stride;++p)check(output[at+p]==197);
                    }
                    ++checked;
                }
            }
        }
    }
    std::cout<<"SDK "<<checked<<" exact decodes: alpha/corrections/coefficient/motion/repairs/repeats, borrowed/owned rows, padded scratch reuse, cache changes, seek, error recovery and guards PASS\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
