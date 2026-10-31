#include "motion_temporal.hpp"
#include "temporal_coefficients.hpp"
#include "temporal_predictive.hpp"
#include "artifact_reconstruction.hpp"
#include "fast_reconstruction.hpp"
#include "edge_upscale.hpp"
#include "fajk/compute.hpp"
#include "fajk/entropy.hpp"
#include "coefficient_overrides.hpp"
#include "timing.hpp"
#include "decode_session.hpp"
#include "fajk/hardware.hpp"
namespace fajk {
std::optional<MotionReference> readMotionRoot(const Packet& packet,uint32_t width,uint32_t height,uint32_t workers,Frame* independentDisplay){
    if(packet.reference||packet.referenceIndex!=UINT64_MAX||packet.data.size()<5||packet.data[4]!=130)return {};
    const auto wrapper=read441(packet.data,width,height);
    if(wrapper.settings.protectText||wrapper.child.size()<28||wrapper.child[4]!=129)return {};
    ByteReader base{wrapper.child};base.bytes(6);const auto surfaces=base.u16();base.bytes(8);const auto baseBudget=base.u32();const auto contours=base.u16(),flags=base.u16();auto coded=base.bytes(base.u32());base.end();
    if(coded.size()>28&&coded[4]==28){ByteReader scaled{coded};scaled.bytes(24);coded=scaled.bytes(scaled.u32());}
    if(coded.size()>7&&coded[4]==55){ByteReader color{coded};color.bytes(7);color.bytes(color.var());coded=color.bytes(color.data.size()-color.pos);}
    if(coded.size()<32||coded[4]!=114)return {};
    ByteReader quantizer{coded.subspan(16,2)};const float residualStep=quantizer.u16()/16.f;
    DeferredUpscale native;native.skipBaseCleanup=true;
    CoefficientBandOverrides overrides;
    auto* captured=coefficientEncodeCapture;
    const bool reuse=captured&&captured->valid&&captured->nativePacket.size()==coded.size()&&
        std::equal(coded.begin(),coded.end(),captured->nativePacket.begin());
    if(reuse){overrides.width=captured->width;overrides.height=captured->height;
        for(unsigned b=0;b<54;++b){overrides.active[b]=1;overrides.integers[b]=captured->bands[b];}}
    ScopedCoefficientBandOverrides scope(reuse?&overrides:coefficientBandOverrides);
    ScopedComputeBackend canonical(ComputeBackend::Cpu);
    auto raw=decodeNestedFrame({wrapper.child,packet.pts,packet.duration},width,height,workers,{},&native,100,0);
    if(reuse)captured->used=true;
    if(!native.flatMetadata.empty()||raw.pixels.size()>32*1024*1024)return {};
    if(!predictiveOpaque(raw))return {};
    MotionReference result{std::move(raw),wrapper.settings,wrapper.budget,native.active?native.surfaces:surfaces,native.active?native.contours:contours,native.active,residualStep};
    if(independentDisplay&&(native.active||!flags))
        *independentDisplay=renderMotionReference(result,packet.pts,packet.duration,width,height,workers,100,{},nullptr,native.active?native.budget:baseBudget);
    return result;
}
Bytes encodeMotionRepairs(const Frame& predicted,const Frame& independent,std::span<const uint32_t> tiles,uint32_t workers){
    predicted.validate();independent.validate();require(predicted.width==independent.width&&predicted.height==independent.height&&!tiles.empty()&&tiles.size()<=32,"Invalid motion repairs.");
    const uint32_t columns=(predicted.width+31)/32,rows=(predicted.height+31)/32;
    for(size_t i=0;i<tiles.size();++i)require(tiles[i]<uint64_t(columns)*rows&&(!i||tiles[i]>tiles[i-1]),"Invalid motion repair order.");
    std::vector<Bytes> coded(tiles.size());
    parallelFor(uint32_t(tiles.size()),workers,[&](uint32_t i){
        const auto x0=tiles[i]%columns*32,y0=tiles[i]/columns*32,w=std::min(32u,predicted.width-x0),h=std::min(32u,predicted.height-y0);const auto area=size_t(w)*h;Bytes delta(area*3);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=(size_t(y0+y)*predicted.width+x0+x)*4,local=size_t(y)*w+x;
            for(unsigned c=0;c<3;++c)delta[c*area+local]=uint8_t(int(independent.pixels[at+c])-predicted.pixels[at+c]);}
        coded[i]=entropyEncode(delta);
    });
    ByteWriter out;out.var(uint32_t(tiles.size()));for(size_t i=0;i<tiles.size();++i){out.var(tiles[i]);out.var(uint32_t(coded[i].size()));out.bytes(coded[i]);}return std::move(out.data);
}
namespace {
void applyMotionRepairs(Frame& image,ByteReader& in,uint32_t workers){
    const uint32_t columns=(image.width+31)/32,rows=(image.height+31)/32,count=in.var();
    require(count>0&&count<=32&&count<=uint64_t(columns)*rows,"Invalid motion repair count.");
    std::vector<uint32_t> tiles(count);std::vector<std::span<const uint8_t>> coded(count);
    for(uint32_t i=0;i<count;++i){tiles[i]=in.var();require(tiles[i]<uint64_t(columns)*rows&&(!i||tiles[i]>tiles[i-1]),"Invalid motion repair tile.");coded[i]=in.bytes(in.var());}in.end();
    parallelFor(count,workers,[&](uint32_t i){
        const auto x0=tiles[i]%columns*32,y0=tiles[i]/columns*32,w=std::min(32u,image.width-x0),h=std::min(32u,image.height-y0);const auto area=size_t(w)*h;
        const auto delta=entropyDecode(coded[i],area*3);require(delta.size()==area*3,"Invalid motion repair pixels.");
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=(size_t(y0+y)*image.width+x0+x)*4,local=size_t(y)*w+x;
            for(unsigned c=0;c<3;++c)image.pixels[at+c]=uint8_t(unsigned(image.pixels[at+c])+delta[c*area+local]);}
    });
}
struct Cache {std::weak_ptr<const Packet> packet;std::shared_ptr<const MotionReference> reference;uint32_t width=0,height=0;size_t retainedBytes()const noexcept{return reference?reference->image.pixels.capacity():0;}};
// A short seek chain can restart from the same root without emitting a new
// keyframe. SequenceReader may then give that root a new packet identity.
// Retain one canonical root by exact payload, independent of display options.
// Its image and payload together are capped at 32 MiB per decoder thread.
struct RootCache {Bytes payload;std::shared_ptr<const MotionReference> reference;uint32_t width=0,height=0;size_t retainedBytes()const noexcept{return payload.capacity()+(reference?reference->image.pixels.capacity():0);}};
bool repeat(std::span<const uint8_t> bytes){
    require(bytes.size()>=8&&bytes.size()<=maxPacket,"Invalid motion packet size.");ByteReader in{bytes};
    require(in.u32()==0x334b4a46&&in.u8()==motionTemporalVersion,"Invalid motion packet magic.");const auto mode=in.u8();require(mode>=1&&mode<=3&&in.u16()==0,"Invalid motion packet header.");
    if(mode==2)in.end();return mode==2;
}
Frame reconstruct(std::span<const uint8_t> bytes,int64_t pts,int64_t duration,const Frame& reference,uint32_t workers){
    require(!repeat(bytes),"Missing motion residual.");ByteReader in{bytes};in.bytes(8);const auto child=in.bytes(in.u32());
    require(child.size()>=20&&child[4]==134&&child[5]==1&&(child[6]==14||child[6]==15||child[6]==16)&&child[7]==0,"Unsupported motion residual.");
    auto image=decodePredictiveTrial({child,pts,duration},reference,workers);
    if(bytes[5]==3)applyMotionRepairs(image,in,workers);else in.end();return image;
}
std::shared_ptr<const MotionReference> resolve(std::shared_ptr<const Packet> packet,uint32_t width,uint32_t height,uint32_t workers,uint32_t depth){
    auto& cache=decodeState<Cache>();auto& rootCache=decodeState<RootCache>();
    require(packet&&packet->data.size()>=8&&depth<coefficientMaxDepth,"Missing or excessive motion reference chain.");
    if(cache.width==width&&cache.height==height&&cache.packet.lock()==packet)return cache.reference;
    std::shared_ptr<const MotionReference> value;
    if(packet->data[4]==motionTemporalVersion){
        const auto parent=resolve(packet->reference,width,height,workers,depth+1);
        if(repeat(packet->data))value=parent;
        else{auto current=std::make_shared<MotionReference>(MotionReference{{},parent->settings,parent->budget,parent->surfaces,parent->contours,parent->scaled});
            current->image=reconstruct(packet->data,packet->pts,packet->duration,parent->image,workers);value=std::move(current);}
    }else if(packet->data[4]==coefficientTemporalVersion){
        // A quality-rejected P frame may use exact coefficient recoding.
        // Consume its existing integer expansion directly; do not entropy
        // encode it again, or cache its empty-band skeleton as a unique image.
        auto expanded=prepareCoefficientTemporal(packet->data,packet->reference,workers,packet);
        expanded.packet.pts=packet->pts;expanded.packet.duration=packet->duration;
        ScopedCoefficientBandOverrides scope(&expanded.coefficients);
        auto root=readMotionRoot(expanded.packet,width,height,workers);
        require(bool(root),"Unsupported coefficient/motion reference.");
        value=std::make_shared<MotionReference>(std::move(*root));
    }else{
        const auto& rootPacket=*packet;
        if(rootCache.width==width&&rootCache.height==height&&rootCache.payload==rootPacket.data)value=rootCache.reference;
        else{
            TimingTimeline timing;
            auto root=readMotionRoot(rootPacket,width,height,workers);require(bool(root),"Unsupported motion reference root.");value=std::make_shared<MotionReference>(std::move(*root));
            timing.mark("decode.motion.root");
            constexpr size_t limit=32*1024*1024;
            const auto imageBytes=value->image.pixels.capacity();
            if(imageBytes<=limit&&rootPacket.data.size()<=limit-imageBytes){
                // This cache is optional: inability to allocate its payload
                // must not discard a successfully decoded frame. Count vector
                // capacity, including retained backing memory, in the limit.
                try{RootCache next{rootPacket.data,value,width,height};
                    if(next.payload.capacity()<=limit-imageBytes)rootCache=std::move(next);else rootCache={};}
                catch(const std::bad_alloc&){rootCache={};}
            }else rootCache={};
        }
    }
    cache={packet,value,width,height};return value;
}
}
Frame decodeMotionTemporal(PacketView packet,uint32_t width,uint32_t height,uint32_t workers,uint32_t deblocking,std::span<uint8_t> destination,DecodePresentation* presentation){
    require(packet.reference&&deblocking<=100,"Missing motion reference.");
    const auto parent=resolve(packet.reference,width,height,workers,0);
    auto current=parent;
    if(!repeat(packet.data)){
        auto updated=std::make_shared<MotionReference>(MotionReference{{},parent->settings,parent->budget,parent->surfaces,parent->contours,parent->scaled});
        updated->image=reconstruct(packet.data,packet.pts,packet.duration,parent->image,workers);
        current=std::move(updated);
    }
    // A caller may edit the payload or parent of a Packet copy while keeping
    // the reader's immutable identity. Never attribute that different image
    // to the original packet: a later dependent frame would reuse it.
    if(packet.keyIdentity&&packet.keyIdentity->reference==packet.reference&&
       packet.keyIdentity->data.size()==packet.data.size()&&
       std::equal(packet.data.begin(),packet.data.end(),packet.keyIdentity->data.begin()))
        decodeState<Cache>()={packet.keyIdentity,current,width,height};
    return renderMotionReference(*current,packet.pts,packet.duration,width,height,workers,deblocking,destination,presentation);
}
Frame renderMotionReference(const MotionReference& reference,int64_t pts,int64_t duration,uint32_t width,uint32_t height,uint32_t workers,uint32_t deblocking,std::span<uint8_t> destination,DecodePresentation* presentation,uint32_t baseCleanupBudget){
    require(deblocking<=100,"Invalid motion display filtering.");
    const auto* current=&reference;
    // A native destination already owns the writable presentation surface.
    // Copy the immutable reference there once, rather than allocate another
    // RGBA image, filter it and then copy the entire image a second time.
    const bool direct=!current->scaled&&!destination.empty();
    Frame raw{current->image.width,current->image.height,pts,duration,direct?Bytes{}:current->image.pixels};
    if(direct){require(destination.size()==current->image.pixels.size(),"Invalid motion output size.");std::copy(current->image.pixels.begin(),current->image.pixels.end(),destination.begin());}
    auto pixels=direct?destination:std::span(raw.pixels);
    const auto cleanupBudget=baseCleanupBudget?baseCleanupBudget:current->budget;
    std::shared_ptr<const ArtifactGuidePlan> artifact;
    if(current->scaled&&current->settings.intensity&&!current->settings.protectText&&uint64_t(width)*height<=4*1024*1024&&computeBackend()==ComputeBackend::Gpu)
        artifact=artifactReconstruction::prepareGpu(pixels,raw.width,raw.height,workers,cleanupBudget,current->surfaces*deblocking/100,current->contours*deblocking/100,width,height,0,current->settings.shapes);
    if(!artifact)artifactReconstruction::restore(pixels,raw.width,raw.height,workers,cleanupBudget,current->surfaces*deblocking/100,current->contours*deblocking/100,width,height,current->settings.shapes);
    if(current->scaled){
        if(current->settings.intensity)return fastReconstruct441(raw,width,height,workers,current->budget,current->settings,destination,nullptr,nullptr,presentation,artifact.get());
        auto result=edgeUpscale(raw,width,height,workers,destination,std::min(1.f,current->contours/100.f),false,false);
        if(current->settings.antialias||current->settings.contrast)reconstruct441(destination.empty()?std::span(result.pixels):destination,width,height,workers,current->budget,current->settings);
        return result;
    }
    reconstruct441(pixels,width,height,workers,current->budget,current->settings);
    return raw;
}
}
