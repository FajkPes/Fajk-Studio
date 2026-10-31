#include "reconstruction441.hpp"
#include "codec_internal.hpp"
#include "fast_reconstruction.hpp"
#include "cleanup_quality.hpp"
#include "tangent_cleanup.hpp"
#include "timing.hpp"
#include "text_guard.hpp"
namespace fajk {
void validate441(const Reconstruction441& s){
    require(s.intensity<=200&&s.strongerTangents<=100&&s.antialias<=100&&s.contrast<=100&&s.style<=1,"Invalid R441 reconstruction controls.");
    require(s.shapes.tolerance>=25&&s.shapes.tolerance<=200&&s.shapes.minimumSize>=25&&s.shapes.minimumSize<=200,"Invalid contour tolerance or minimum scale.");
}
Reconstruction441Packet read441(std::span<const uint8_t> data,uint32_t width,uint32_t height){
    require(data.size()<=maxPacket,"R441 packet exceeds limit.");ByteReader in{data};
    require(in.u32()==frameMagic&&in.u8()==reconstruction441Version&&in.u8()==1&&in.u16()==0,"Invalid R441 header.");
    require(in.u32()==width&&in.u32()==height,"R441 dimensions differ.");
    Reconstruction441Packet result;result.budget=in.u32();
    require(result.budget>=256&&result.budget<=uint64_t(maxPacket)+168,"Invalid R441 budget.");
    auto& s=result.settings;s.intensity=in.u16();s.strongerTangents=in.u16();s.antialias=in.u16();s.contrast=in.u16();
    const auto flags=in.u16(),shapeWord=in.u16();require(flags<=7,"Invalid R441 flags.");s.protectText=bool(flags&1);s.style=(flags>>1)&1;s.fixedIntensity=bool(flags&4);
    if(shapeWord)s.shapes={uint32_t(shapeWord&255),uint32_t(shapeWord>>8)};
    validate441(s);
    result.child=in.bytes(in.u32());in.end();
    // A single R440 base prevents recursive wrappers and duplicate R441 cleanup.
    require(result.child.size()>=28&&result.child[4]==129&&result.child[5]==12,"R441 requires a native R440 base.");
    return result;
}
void update441(Packet& packet,const Reconstruction441& s){
    validate441(s);ByteReader in{packet.data};in.bytes(8);const auto w=in.u32(),h=in.u32();pixelBytes(w,h);read441(packet.data,w,h);
    ByteWriter out;out.u16(uint16_t(s.intensity));out.u16(uint16_t(s.strongerTangents));out.u16(uint16_t(s.antialias));out.u16(uint16_t(s.contrast));out.u16(uint16_t((s.protectText?1:0)|(s.style<<1)|(s.fixedIntensity?4:0)));out.u16(shapeTuningWord(s));
    std::copy(out.data.begin(),out.data.end(),packet.data.begin()+20);
}
void reconstruct441(std::span<uint8_t> pixels,uint32_t w,uint32_t h,uint32_t workers,uint32_t budget,const Reconstruction441& s,const DecodedTextProtection* protectedText){
    require(pixels.size()==pixelBytes(w,h),"Invalid R441 output.");validate441(s);
    const auto quality=cleanupQuality(budget,w,h);
    const float amount=skirtBlend(s,quality);
    const float aa=quality.antialias*s.antialias/100.f,contrast=s.contrast/100.f;
    if(!amount&&!aa&&!contrast){if(s.protectText&&protectedText)restoreDecodedText(pixels,*protectedText,w,h);return;}
    require(hasAvx2(),"R441 reconstruction requires AVX2.");
    TimingTimeline timing;thread_local Bytes snapshot;Bytes large;
    auto& decoded=pixels.size()<=32*1024*1024?snapshot:large;
    if(s.protectText)decoded.assign(pixels.begin(),pixels.end());
    recursiveContourCleanup(pixels,w,h,workers,amount,s.strongerTangents/100.f,s.protectText?std::span<const uint8_t>(decoded):std::span<const uint8_t>{});
    timing.mark("decode.fast441.recurrence");
    if(aa)contourAntialias(pixels,w,h,workers,aa);
    // The reviewed contrast variants apply bounded contrast after AA.
    if(contrast)boundedContourContrast(pixels,w,h,workers,contrast);
    timing.mark("decode.r441.aa_contrast");
    if(s.protectText){if(protectedText)restoreDecodedText(pixels,*protectedText,w,h);else preserveDecodedText(pixels,decoded,w,h,workers);}
    timing.mark("decode.r441.text_protection");
}
}
