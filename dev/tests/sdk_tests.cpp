#include "fajk/decode.h"
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "../src/temporal.hpp"
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
    explicit Fixture(bool alpha){
        path=std::filesystem::temp_directory_path()/(L"fajk-sdk-"+std::to_wstring(GetCurrentProcessId())+(alpha?L"-alpha.fajkv":L"-correction.fajkv"));
        Frame source{259,197,0,333333,Bytes(pixelBytes(259,197))};
        for(uint32_t y=0;y<source.height;++y)for(uint32_t x=0;x<source.width;++x){const size_t at=(size_t(y)*source.width+x)*4;
            source.pixels[at]=uint8_t(x+y);source.pixels[at+1]=uint8_t(x*3+y/2);source.pixels[at+2]=uint8_t(x/3+y*5);source.pixels[at+3]=alpha?uint8_t(x+y):255;}
        EncodeOptions options;options.speed=EncodeSpeed::Fast;options.targetBytes=12000;options.threads=2;
        auto codec=makeCodec(CodecId::Fajk);TemporalEncoder temporal(options,120,alpha?0:3,10);
        SequenceInfo info{source.width,source.height,30,1,CodecId::Fajk,1,0,0,true};SequenceWriter writer(path,info,true);
        for(uint64_t n=0;n<3;++n){auto frame=source;frame.pts=int64_t(n)*frame.duration;
            if(n==1){
                if(!alpha)for(size_t p=0;p<frame.pixels.size();p+=4)for(size_t c=0;c<3;++c)frame.pixels[p+c]=uint8_t(std::min(255,int(frame.pixels[p+c])+2));
                for(uint32_t y=192;y<197;++y)for(uint32_t x=256;x<259;++x)frame.pixels[(size_t(y)*259+x)*4]^=127;
                if(alpha)frame.pixels.back()^=1;
            }else if(n==2)for(size_t p=0;p<frame.pixels.size();p+=4)for(size_t c=0;c<3;++c)frame.pixels[p+c]^=255;
            auto packet=temporal.encode(frame,n);if(!packet){packet=codec->encode(frame,options);temporal.commitKey(*packet,n);}
            if(n==1)check(packet->data[4]==(alpha?temporalPacketVersion:temporalCorrectionVersion));
            writer.append(*packet);
        }
        writer.finish();
    }
    ~Fixture(){std::error_code error;std::filesystem::remove(path,error);}
};
}
int main(){try{
    check(fajk_decode_abi_version()==1);uint64_t checked=0;
    for(bool alpha:{false,true}){
        Fixture fixture(alpha);SequenceReader reader(fixture.path);auto codec=makeCodec(CodecId::Fajk);
        for(uint32_t workers:{1,2,10})for(uint32_t backend:{0,1}){
            char error[256];const auto utf8=fixture.path.u8string();
            std::unique_ptr<fajk_decoder,decltype(&fajk_close)> decoder(fajk_open(reinterpret_cast<const char*>(utf8.c_str()),error,sizeof(error)),fajk_close);
            check(bool(decoder));fajk_sequence_info info{};check(fajk_get_info(decoder.get(),&info)==-1);
            info.struct_size=sizeof(info);check(fajk_get_info(decoder.get(),&info)==0&&info.width==259&&info.height==197&&info.frame_count==3);
            check(fajk_set_threads(decoder.get(),workers)==0&&fajk_set_backend(decoder.get(),backend)==0);
            codec->setThreads(workers);codec->setComputeBackend(ComputeBackend(backend));
            for(uint32_t padding:{0,1,28})for(uint32_t setting:{0,1,2}){
                const uint32_t deblocking=setting==1?0:100,sharpening=setting*50;
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
    std::cout<<"SDK "<<checked<<" exact decodes: alpha/corrections, borrowed/owned rows, padded scratch reuse, cache changes, seek, error recovery and guards PASS\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
