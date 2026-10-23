#include "fajk/codec.hpp"
#include "decode_internal.hpp"
namespace fajk {
Packet encodeFrame(const Frame&,const EncodeOptions&);
Frame decodeFrame(const Packet&,uint32_t,uint32_t);
Frame decodeFrameWithThreads(const Packet&,uint32_t,uint32_t,uint32_t);
namespace {
class FajkCodec final:public FrameCodec {
    ComputeBackend backend_=ComputeBackend::Cpu;
    uint32_t threads_=0;
    uint32_t deblocking_=100;
    uint32_t sharpening_=0;
public:
    Packet encode(const Frame& f,const EncodeOptions& o)override{return encodeFrame(f,o);}
    Frame decode(const Packet& p,uint32_t w,uint32_t h)override{return decodeFrameWithSettings(p,w,h,threads_,deblocking_,sharpening_,true,backend_);}
    void setComputeBackend(ComputeBackend value)override{require(value==ComputeBackend::Cpu||value==ComputeBackend::Gpu,"Invalid compute backend.");backend_=value;}
    ComputeBackend backend()const override{return backend_;}
    void setThreads(uint32_t threads)override{threads_=threads;}
    void setDeblocking(uint32_t percent)override{require(percent<=100,"Deblocking must be 0–100 percent.");deblocking_=percent;}
    void setSharpening(uint32_t percent)override{require(percent<=100,"Sharpening must be 0–100 percent.");sharpening_=percent;}
};
}
std::unique_ptr<FrameCodec> makeCodec(CodecId id){require(id==CodecId::Fajk,"Unsupported codec.");return std::make_unique<FajkCodec>();}
}
