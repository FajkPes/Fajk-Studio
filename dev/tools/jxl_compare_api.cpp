// Developer-only JPEG XL C API adapter. No dependency in the shipping codec.
#include <jxl/encode.h>
#include <jxl/decode.h>
#include <jxl/color_encoding.h>
#include <jxl/thread_parallel_runner.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>
#include <limits>

namespace {
thread_local std::string error;
struct Runner {
    void* value=nullptr; unsigned threads=0;
    ~Runner(){if(value)JxlThreadParallelRunnerDestroy(value);}
    void* get(unsigned n){if(!value || n!=threads){if(value)JxlThreadParallelRunnerDestroy(value);value=JxlThreadParallelRunnerCreate(nullptr,n);threads=n;}
        if(!value)throw std::runtime_error("JXL runner allocation failed");return value;}
};
thread_local Runner runner;
void check(bool v,const char* message){if(!v)throw std::runtime_error(message);}
}
extern "C" {
struct CompareBuffer {void* data;size_t size;};
__declspec(dllexport) const char* compare_jxl_error(){return error.c_str();}
__declspec(dllexport) unsigned compare_jxl_version(){return JxlEncoderVersion();}
__declspec(dllexport) void compare_jxl_free(void* p){std::free(p);}
__declspec(dllexport) int compare_jxl_encode(const void* rgb,unsigned w,unsigned h,float distance,int effort,unsigned threads,int resampling,CompareBuffer* out){
    try {
        error.clear();*out={};
        check(rgb && w && h && uint64_t(w)*h<=64ull*1024*1024,"Invalid input");
        std::unique_ptr<JxlEncoder,decltype(&JxlEncoderDestroy)> enc(JxlEncoderCreate(nullptr),JxlEncoderDestroy);
        check(bool(enc),"JXL encoder allocation failed");
        check(JxlEncoderSetParallelRunner(enc.get(),JxlThreadParallelRunner,runner.get(threads))==JXL_ENC_SUCCESS,"JXL runner failed");
        check(JxlEncoderUseContainer(enc.get(),JXL_TRUE)==JXL_ENC_SUCCESS,"JXL container failed");
        JxlBasicInfo info;JxlEncoderInitBasicInfo(&info);info.xsize=w;info.ysize=h;info.bits_per_sample=8;
        info.num_color_channels=3;info.num_extra_channels=0;info.uses_original_profile=JXL_FALSE;
        check(JxlEncoderSetBasicInfo(enc.get(),&info)==JXL_ENC_SUCCESS,"JXL basic info failed");
        JxlColorEncoding color;JxlColorEncodingSetToSRGB(&color,JXL_FALSE);
        check(JxlEncoderSetColorEncoding(enc.get(),&color)==JXL_ENC_SUCCESS,"JXL color failed");
        auto* frame=JxlEncoderFrameSettingsCreate(enc.get(),nullptr);
        check(JxlEncoderSetFrameDistance(frame,distance)==JXL_ENC_SUCCESS,"Invalid JXL distance");
        check(JxlEncoderFrameSettingsSetOption(frame,JXL_ENC_FRAME_SETTING_EFFORT,effort)==JXL_ENC_SUCCESS,"Invalid JXL effort");
        check(JxlEncoderFrameSettingsSetOption(frame,JXL_ENC_FRAME_SETTING_RESAMPLING,resampling)==JXL_ENC_SUCCESS,"Invalid JXL resampling");
        JxlPixelFormat format{3,JXL_TYPE_UINT8,JXL_NATIVE_ENDIAN,0};
        check(JxlEncoderAddImageFrame(frame,&format,rgb,size_t(w)*h*3)==JXL_ENC_SUCCESS,"JXL frame failed");
        JxlEncoderCloseInput(enc.get());
        std::vector<uint8_t> bytes(65536);size_t used=0;
        for(;;){auto* next=bytes.data()+used;size_t available=bytes.size()-used;
            auto status=JxlEncoderProcessOutput(enc.get(),&next,&available);used=size_t(next-bytes.data());
            if(status==JXL_ENC_SUCCESS)break;
            check(status==JXL_ENC_NEED_MORE_OUTPUT,"JXL encode failed");bytes.resize(bytes.size()*2);
        }
        out->data=std::malloc(used);check(out->data!=nullptr,"JXL output allocation failed");
        std::memcpy(out->data,bytes.data(),used);out->size=used;return 0;
    }catch(const std::exception& e){error=e.what();return -1;}
}
__declspec(dllexport) int compare_jxl_decode(const void* bytes,size_t size,unsigned threads,CompareBuffer* out,unsigned* w,unsigned* h){
    try {
        error.clear();*out={};
        std::unique_ptr<JxlDecoder,decltype(&JxlDecoderDestroy)> dec(JxlDecoderCreate(nullptr),JxlDecoderDestroy);
        check(bool(dec),"JXL decoder allocation failed");
        check(JxlDecoderSetParallelRunner(dec.get(),JxlThreadParallelRunner,runner.get(threads))==JXL_DEC_SUCCESS,"JXL decode runner failed");
        check(JxlDecoderSubscribeEvents(dec.get(),JXL_DEC_BASIC_INFO|JXL_DEC_FULL_IMAGE)==JXL_DEC_SUCCESS,"JXL events failed");
        check(JxlDecoderSetInput(dec.get(),static_cast<const uint8_t*>(bytes),size)==JXL_DEC_SUCCESS,"JXL input failed");JxlDecoderCloseInput(dec.get());
        std::unique_ptr<void,decltype(&std::free)> pixels(nullptr,std::free);size_t length=0;
        JxlPixelFormat format{4,JXL_TYPE_UINT8,JXL_NATIVE_ENDIAN,0};
        for(;;){auto status=JxlDecoderProcessInput(dec.get());
            if(status==JXL_DEC_BASIC_INFO){JxlBasicInfo info;check(JxlDecoderGetBasicInfo(dec.get(),&info)==JXL_DEC_SUCCESS,"JXL decoded info failed");*w=info.xsize;*h=info.ysize;}
            else if(status==JXL_DEC_NEED_IMAGE_OUT_BUFFER){check(JxlDecoderImageOutBufferSize(dec.get(),&format,&length)==JXL_DEC_SUCCESS,"JXL size failed");
                pixels.reset(std::malloc(length));check(bool(pixels),"JXL image allocation failed");
                check(JxlDecoderSetImageOutBuffer(dec.get(),&format,pixels.get(),length)==JXL_DEC_SUCCESS,"JXL image buffer failed");}
            else if(status==JXL_DEC_FULL_IMAGE){}
            else if(status==JXL_DEC_SUCCESS){check(bool(pixels),"JXL missing pixels");out->data=pixels.release();out->size=length;return 0;}
            else throw std::runtime_error("JXL decode failed");
        }
    }catch(const std::exception& e){error=e.what();return -1;}
}
}
