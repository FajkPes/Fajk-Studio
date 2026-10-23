#include "fajk/decode.h"
#include "fajk/container.hpp"
#include "decode_internal.hpp"
#include <cstring>
#include <memory>
#include <algorithm>
struct fajk_decoder {fajk::SequenceReader reader;std::string error;fajk::Bytes paddedScratch;fajk::ComputeBackend backend=fajk::ComputeBackend::Cpu;uint32_t threads=0,deblocking=100,sharpening=0;explicit fajk_decoder(const fajk::Path& p):reader(p){};};
namespace {
void copyError(char* dest,size_t n,const char* text){if(dest&&n){size_t len=std::min(n-1,std::strlen(text));std::memcpy(dest,text,len);dest[len]=0;}}
template<class F>int run(fajk_decoder* d,F f){if(!d)return -1;try{f();d->error.clear();return 0;}catch(const std::exception& e){d->error=e.what();return -1;}catch(...){d->error="Unknown decode failure.";return -1;}}
}
uint32_t fajk_decode_abi_version(){return 1;}
fajk_decoder* fajk_open(const char* path,char* error,size_t capacity){try{fajk::require(path&&*path,"Path is empty.");auto d=std::make_unique<fajk_decoder>(std::filesystem::u8path(path));copyError(error,capacity,"");return d.release();}catch(const std::exception& e){copyError(error,capacity,e.what());return nullptr;}catch(...){copyError(error,capacity,"Unknown open failure.");return nullptr;}}
void fajk_close(fajk_decoder* d){delete d;}
int fajk_get_info(fajk_decoder* d,fajk_sequence_info* out){return run(d,[&]{fajk::require(out&&out->struct_size==sizeof(*out),"Invalid info structure size.");const auto& i=d->reader.info();*out={sizeof(*out),i.width,i.height,i.fpsNum,i.fpsDen,i.sourceStep,i.count,i.duration};});}
int fajk_decode_frame(fajk_decoder* d,uint64_t index,uint8_t* rgba,size_t capacity,size_t stride,int64_t* pts,int64_t* duration){return run(d,[&]{const auto& i=d->reader.info();size_t row=size_t(i.width)*4;fajk::require(rgba&&stride>=row&&stride<=SIZE_MAX/i.height&&capacity>=stride*(i.height-1)+row,"RGBA output buffer is too small.");auto p=d->reader.read(index);
    if(stride==row)fajk::decodeFrameToBuffer(p,i.width,i.height,d->threads,d->deblocking,{rgba,fajk::pixelBytes(i.width,i.height)},d->sharpening,true,d->backend);
    else{
        // Padded destinations need a contiguous surface for reconstruction and
        // postfilters. Keep one bounded scratch per serialized handle instead
        // of allocating and zeroing a new full image on every video frame.
        fajk::Bytes large;const auto size=fajk::pixelBytes(i.width,i.height);
        auto& scratch=size<=64*1024*1024?d->paddedScratch:large;scratch.resize(size);
        fajk::decodeFrameToBuffer(p,i.width,i.height,d->threads,d->deblocking,scratch,d->sharpening,true,d->backend);
        for(uint32_t y=0;y<i.height;++y)std::memcpy(rgba+size_t(y)*stride,scratch.data()+size_t(y)*row,row);
    }
    if(pts)*pts=p.pts;if(duration)*duration=p.duration;});}
int fajk_set_threads(fajk_decoder* d,uint32_t threads){return run(d,[&]{d->threads=threads;});}
int fajk_set_backend(fajk_decoder* d,uint32_t value){return run(d,[&]{fajk::require(value<=1,"Backend must be 0 (CPU) or 1 (GPU).");d->backend=fajk::ComputeBackend(value);});}
int fajk_set_deblocking(fajk_decoder* d,uint32_t percent){return run(d,[&]{fajk::require(percent<=100,"Deblocking must be 0–100 percent.");d->deblocking=percent;});}
int fajk_set_sharpening(fajk_decoder* d,uint32_t percent){return run(d,[&]{fajk::require(percent<=100,"Sharpening must be 0–100 percent.");d->sharpening=percent;});}
int fajk_frame_at_time(fajk_decoder* d,int64_t pts,uint64_t* index){return run(d,[&]{fajk::require(index,"Missing index pointer.");*index=d->reader.frameAt(pts);});}
const char* fajk_last_error(fajk_decoder* d){return d?d->error.c_str():"Invalid decoder handle.";}
