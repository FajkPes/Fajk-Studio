#include "reconstruction441.hpp"
#include "codec_internal.hpp"
#include "wavelet.hpp"
#include "color_prediction.hpp"
#include "flat_regions.hpp"
#include "decode_internal.hpp"
#include "edge_upscale.hpp"
#include "artifact_reconstruction.hpp"
#include "fast_reconstruction.hpp"
#include "fajk/hardware.hpp"
#include "timing.hpp"
#include "fajk/compute.hpp"
#include "temporal.hpp"
#include "temporal_coefficients.hpp"
#include "motion_temporal.hpp"
#include "final_sharpen.hpp"
#include "decode_session.hpp"
#include "decode_presentation.hpp"
#include "gpu_compute.hpp"
#include <cstring>

namespace fajk {
namespace {
Frame decodeLossless(PacketView packet, uint32_t width, uint32_t height, std::span<uint8_t> destination) {
    ByteReader in{packet.data};
    require(in.u32() == frameMagic && in.u8() == 4, "Invalid lossless header.");
    const auto mode = in.u8(), quality = in.u8(), flags = in.u8();
    require((mode == 1 || mode == 2) && quality >= 1 && quality <= 100 && !flags && in.u32() == width &&
                in.u32() == height,
            "Invalid lossless options.");
    Frame result{width, height, packet.pts, packet.duration,
                 destination.empty() ? Bytes(pixelBytes(width, height)) : Bytes{}};
    const auto pixels = destination.empty() ? std::span(result.pixels) : destination;
    if (mode == 1) {
        for (int c = 0; c < 4; ++c)
            undoPredict(readStream(in, size_t(width) * height), pixels, width, height, c);
        in.end();
        return result;
    }
    const auto count = in.u16();
    require(count > 0 && count <= 4096, "Invalid palette size.");
    std::array<uint32_t, 4096> palette{};
    for (unsigned i = 0; i < count; ++i)
        palette[i] = in.u32();
    const auto area = size_t(width) * height;
    auto low = readStream(in, area);
    Bytes high;
    if (count > 256)
        high = readStream(in, area);
    require(low.size() == area && (count <= 256 || high.size() == area), "Invalid palette indices.");
    in.end();
    std::vector<uint16_t> previous(width);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const auto i = size_t(y) * width + x;
            const uint16_t value = count <= 256 ? uint8_t(low[i] + previous[x])
                                                : uint16_t(uint16_t(low[i] | (uint16_t(high[i]) << 8)) + previous[x]);
            require(value < count, "Palette index out of range.");
            previous[x] = value;
            std::memcpy(pixels.data() + i * 4, &palette[value], 4);
        }
    return result;
}
Frame dispatchDecode(PacketView, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, std::span<uint8_t>, DeferredUpscale* = nullptr,DecodePresentation* = nullptr);
struct TemporalKeyCache {
    std::weak_ptr<const Packet> packet;Frame image;
    uint32_t workers=0,deblocking=0,sharpening=0;ComputeBackend backend=ComputeBackend::Cpu;
    size_t retainedBytes()const noexcept{return image.pixels.capacity();}
};
TemporalKeyCache& temporalKeyCache(){return decodeState<TemporalKeyCache>();}
void rememberTemporalKey(PacketView packet,std::span<const uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,uint32_t deblocking){
    if(pixels.size()>64*1024*1024)return;
    const bool coefficientRepeat=packet.reference&&packet.data.size()==8&&(packet.data[4]==coefficientTemporalVersion||packet.data[4]==motionTemporalVersion)&&packet.data[5]==2;
    const bool chainFrame=packet.data[4]==coefficientTemporalVersion||packet.data[4]==motionTemporalVersion;
    if(!coefficientRepeat&&(!packet.keyIdentity||(packet.reference&&(!chainFrame||!packet.cachePresentation))))return;
    // Public C++ callers can mutate their packet copy. Such data must never
    // poison the cache for the reader's different immutable key payload.
    if(!coefficientRepeat&&(packet.reference!=packet.keyIdentity->reference||packet.data.size()!=packet.keyIdentity->data.size()||!std::equal(packet.data.begin(),packet.data.end(),packet.keyIdentity->data.begin())))return;
    auto& cache=temporalKeyCache();
    if(coefficientRepeat&&cache.packet.lock()==packet.reference&&cache.image.width==width&&cache.image.height==height&&cache.workers==workers&&cache.deblocking==deblocking&&cache.sharpening==0&&cache.backend==computeBackend())return;
    cache.image.width=width;cache.image.height=height;cache.image.pts=packet.pts;cache.image.duration=packet.duration;
    cache.image.pixels.assign(pixels.begin(),pixels.end());cache.packet=coefficientRepeat?packet.reference:packet.keyIdentity;
    cache.workers=workers;cache.deblocking=deblocking;cache.sharpening=0;cache.backend=computeBackend();
}
Frame decodeTemporal(PacketView packet,uint32_t width,uint32_t height,uint32_t workers,uint32_t deblocking,uint32_t sharpening,std::span<uint8_t> destination){
    ByteReader in{packet.data};
    require(in.u32()==frameMagic,"Invalid temporal magic.");
    const auto version=in.u8(),mode=in.u8();const auto tileField=in.u16();
    const bool correction=version==temporalCorrectionVersion&&mode==4&&tileField==temporalCorrectionTile;
    require(correction||(version==temporalPacketVersion&&mode==1&&tileField==0),"Invalid temporal header.");
    const uint32_t tileSize=correction?temporalCorrectionTile:temporalTile;
    if(correction){deblocking=100;sharpening=0;}
    require(in.u32()==width&&in.u32()==height,"Temporal dimensions differ.");
    const uint32_t count=in.u32(),cols=(width+tileSize-1)/tileSize,rows=(height+tileSize-1)/tileSize;
    require(count<=uint64_t(cols)*rows&&count<=(in.data.size()-in.pos)/8,"Invalid temporal tile count.");
    require(packet.reference&&packet.reference->data.size()>=5&&!packet.reference->reference&&packet.reference->referenceIndex==UINT64_MAX&&packet.reference->data[4]!=temporalPacketVersion&&packet.reference->data[4]!=temporalCorrectionVersion,"Missing independent temporal reference.");
    auto& cache=temporalKeyCache();
    const auto backend=computeBackend();
    const auto previous=cache.packet.lock();
    const bool match=previous==packet.reference&&cache.image.width==width&&cache.image.height==height&&cache.workers==workers&&cache.deblocking==deblocking&&cache.sharpening==sharpening&&cache.backend==backend;
    Frame uncached;
    const Frame* anchor=nullptr;
    if(match)anchor=&cache.image;
    else{
        auto decoded=dispatchDecode(*packet.reference,width,height,workers,deblocking,sharpening,{});
        if(decoded.pixels.size()<=64*1024*1024){
            cache.image=std::move(decoded);cache.packet=packet.reference;cache.workers=workers;
            cache.deblocking=deblocking;cache.sharpening=sharpening;cache.backend=backend;anchor=&cache.image;
        }else{uncached=std::move(decoded);anchor=&uncached;}
    }
    Frame result{width,height,packet.pts,packet.duration,destination.empty()?Bytes(pixelBytes(width,height)):Bytes{}};
    auto output=destination.empty()?std::span(result.pixels):destination;
    std::memcpy(output.data(),anchor->pixels.data(),output.size());
    uint32_t last=UINT32_MAX;
    for(uint32_t n=0;n<count;++n){
        const auto tile=in.u32(),size=in.u32();
        require(tile<uint64_t(cols)*rows&&(n==0||tile>last),"Invalid temporal tile order.");last=tile;
        const uint32_t x=tile%cols*tileSize,y=tile/cols*tileSize,w=std::min(tileSize,width-x),h=std::min(tileSize,height-y);
        const size_t area=size_t(w)*h;
        const auto differences=entropyDecode(in.bytes(size),area*8);
        require(differences.size()==area*8,"Invalid temporal difference size.");
        for(uint32_t dy=0;dy<h;++dy)for(uint32_t dx=0;dx<w;++dx){
            const size_t pixel=(size_t(y+dy)*width+x+dx)*4,at=size_t(dy)*w+dx;
            for(size_t c=0;c<4;++c){
                const auto value=uint32_t(differences[c*area+at])|(uint32_t(differences[(c+4)*area+at])<<8);
                require(value<=510,"Invalid temporal difference.");
                output[pixel+c]=uint8_t(std::clamp(int(anchor->pixels[pixel+c])+int(value)-255,0,255));
            }
        }
    }
    in.end();return result;
}
void prepareTextProtection(DeferredUpscale* deferred,std::span<const uint8_t> pixels,uint32_t width,uint32_t height,uint32_t workers,uint32_t budget,uint32_t surfaces,uint32_t contours,uint32_t sourceWidth=0,uint32_t sourceHeight=0){
    if(!deferred||!deferred->protectText||deferred->skipBaseCleanup)return;
    const auto rw=sourceWidth?sourceWidth:width,rh=sourceHeight?sourceHeight:height;
    const auto rate=artifactReconstruction::rateStrength(budget,rw,rh);
    const double equivalent=double(budget)*double(2560*1440)/(double(rw)*rh);
    // Mild geometry uses the established late text guard. The early glyph
    // reference is needed for the stronger curved reconstruction; analyzing
    // it at moderate/high density added cost with no material visual gain.
    if(!surfaces&&artifactReconstruction::contourStrength(rate,equivalent,contours)<=.8f)return;
    TimingTimeline timing;deferred->protectedText=captureDecodedText(pixels,width,height,workers);timing.mark("decode.text_analysis");
}
Frame decodeScaled(PacketView packet, uint32_t width, uint32_t height, uint32_t workers,
                   std::span<uint8_t> destination, DeferredUpscale* deferred, uint32_t deblocking, uint32_t sharpening) {
    ByteReader in{packet.data};
    require(in.u32() == frameMagic && in.u8() == 28 && in.u8() == 130 && in.u16() == 0, "Unsupported scaled packet.");
    require(in.u32() == width && in.u32() == height, "Scaled dimensions differ.");
    const auto iw = in.u32(), ih = in.u32();
    pixelBytes(iw, ih);
    require(iw <= width && ih <= height, "Invalid working dimensions.");
    const auto encoded = in.bytes(in.u32());
    const auto budget = in.u32();
    const auto surfaces = in.u16(), contours = in.u16();
    in.end();
    require(budget >= 256 && budget <= maxPacket + 168 && surfaces <= 200 && contours <= 200,
            "Invalid reconstruction controls.");
    require(encoded.size() >= 16 && (encoded[4] == 55 || encoded[4] == 114), "Unsupported scaled child.");
    auto nativeBytes=encoded;
    if(encoded[4]==55){ByteReader color{encoded};color.bytes(7);color.bytes(color.var());nativeBytes=color.bytes(color.data.size()-color.pos);}
    // The native decoder writes alpha=255 for an opaque packet. Validate that
    // invariant from its header before decoding instead of rescanning all RGBA.
    require(nativeBytes.size()>=8&&nativeBytes[4]==114&&(nativeBytes[7]&1)&&hasAvx2(),
            "R441 scaled reconstruction requires opaque AVX2 output.");
    struct WorkingCache {Bytes pixels;size_t retainedBytes()const noexcept{return pixels.capacity();}};auto& cachedWorking=decodeState<WorkingCache>().pixels;
    Bytes largeWorking;
    auto& working = pixelBytes(iw, ih) <= 32*1024*1024 ? cachedWorking : largeWorking;
    working.resize(pixelBytes(iw, ih));
    GpuDecodedSource decodedSource;
    const bool retainNative=deferred&&deferred->allowGpuArtifact&&!surfaces&&contours&&uint64_t(iw)*ih>=512*512&&gpuArtifactSupported();
    Frame low;
    {ScopedGpuDecodedSource capture(retainNative?&decodedSource:nullptr);
        low=dispatchDecode({encoded, packet.pts, packet.duration}, iw, ih, workers, deblocking, sharpening, working);}
    low.pixels = std::move(working);
    prepareTextProtection(deferred,low.pixels,iw,ih,workers,budget,surfaces,contours,width,height);
    if(!deferred||!deferred->skipBaseCleanup){
        if(deferred&&deferred->allowGpuArtifact){
            const auto stamp=decodedSource.pixels==low.pixels.data()&&decodedSource.bytes==low.pixels.size()?decodedSource.stamp:0;
            deferred->artifact=artifactReconstruction::prepareGpu(low.pixels,iw,ih,workers,budget,surfaces,contours,width,height,stamp,deferred->shapes);
        }
        if(!deferred||!deferred->artifact)
            artifactReconstruction::restore(low.pixels, iw, ih, workers, budget, surfaces, contours, width, height,deferred?deferred->shapes:ContourTuning{});
    }
    if(deferred&&deferred->protectedText.width)refreshDecodedTextFallback(deferred->protectedText,low.pixels,iw,ih);
    if(deferred){deferred->active=true;deferred->budget=budget;deferred->surfaces=surfaces;deferred->contours=contours;deferred->recycle=&working==&cachedWorking?&cachedWorking:nullptr;return low;}
    auto result = edgeUpscale(low, width, height, workers, destination, std::min(1.f, contours / 100.f), false, false);
    working=std::move(low.pixels);
    return result;
}
Frame decodeBase(PacketView packet, uint32_t width, uint32_t height, uint32_t workers, std::span<uint8_t> destination, DeferredUpscale* deferred, uint32_t deblocking, uint32_t sharpening) {
    ByteReader in{packet.data};
    require(in.u32() == frameMagic && in.u8() == 129 && in.u8() == 12, "Unsupported reconstruction packet.");
    const auto surfaces = in.u16();
    require(surfaces <= 200 && in.u32() == width && in.u32() == height, "Invalid reconstruction dimensions.");
    const auto budget = in.u32();
    const auto contours = in.u16(), flags = in.u16();
    require(budget >= 256 && budget <= maxPacket + 168 && contours <= 200 && flags <= 1,
            "Invalid reconstruction controls.");
    const auto child = in.bytes(in.u32());
    in.end();
    require(child.size() >= 16 && (child[4] == 77 || child[4] == 28 || child[4] == 55 || child[4] == 114),
            "Unsupported reconstruction child.");
    auto result = dispatchDecode({child, packet.pts, packet.duration}, width, height, workers, deblocking, sharpening, destination,deferred);
    require(!deferred||!deferred->active||flags==1,"Scaled reconstruction must declare working cleanup.");
    if (!flags&&(!deferred||!deferred->skipBaseCleanup)) {
        require(hasAvx2(), "R441 reconstruction requires AVX2.");
        prepareTextProtection(deferred,destination.empty()?std::span<const uint8_t>(result.pixels):std::span<const uint8_t>(destination),width,height,workers,budget,surfaces,contours);
        artifactReconstruction::restore(destination.empty() ? std::span(result.pixels) : destination, width, height,
                                        workers, budget, surfaces, contours,0,0,deferred?deferred->shapes:ContourTuning{});
        if(deferred&&deferred->protectedText.width)refreshDecodedTextFallback(deferred->protectedText,destination.empty()?std::span<const uint8_t>(result.pixels):std::span<const uint8_t>(destination),width,height);
    }
    return result;
}
Frame dispatchDecode(PacketView packet, uint32_t width, uint32_t height, uint32_t threads, uint32_t deblocking,
                     uint32_t sharpening, std::span<uint8_t> destination, DeferredUpscale* deferred,DecodePresentation* presentation) {
    const auto size = pixelBytes(width, height);
    require(destination.empty() || destination.size() == size, "Invalid decode destination.");
    require(deblocking <= 100 && sharpening <= 100 && packet.data.size() <= maxPacket && packet.data.size() >= 5,
            "Invalid decode options or packet.");
    // The outer entry point has already selected a safe worker budget for the
    // largest image. Recursive wrappers keep it instead of querying RAM again.
    const auto workers = threads;
    switch (packet.data[4]) {
    case motionTemporalVersion:
    case coefficientTemporalVersion: {
        require(!deferred&&packet.reference&&packet.reference->data.size()>4,"Missing coefficient reference.");
        if(packet.data.size()==8&&packet.data[5]==2){
            ByteReader in{packet.data};require(in.u32()==frameMagic&&in.u8()==packet.data[4]&&in.u8()==2&&in.u16()==0,"Invalid temporal repeat.");
            const auto& cache=temporalKeyCache();
            if(cache.packet.lock()==packet.reference&&cache.image.width==width&&cache.image.height==height&&cache.workers==workers&&cache.deblocking==deblocking&&cache.sharpening==0&&cache.backend==computeBackend()){
                Frame result{width,height,packet.pts,packet.duration,destination.empty()?cache.image.pixels:Bytes{}};
                if(!destination.empty())std::copy(cache.image.pixels.begin(),cache.image.pixels.end(),destination.begin());
                return result;
            }
            // A repeat means the same displayed picture, including the
            // independent/coefficient reconstruction path. Resolving only a
            // motion root's raw pixels changed key/hold images after a seek.
            auto ancestor=packet.reference;uint32_t depth=0;
            while(ancestor&&ancestor->data.size()==8&&ancestor->data[5]==2&&
                (ancestor->data[4]==motionTemporalVersion||ancestor->data[4]==coefficientTemporalVersion)){
                require(++depth<coefficientMaxDepth,"Excessive temporal repeat chain.");
                ByteReader header{ancestor->data};require(header.u32()==frameMagic&&header.u8()==ancestor->data[4]&&header.u8()==2&&header.u16()==0,"Invalid repeated reference.");
                ancestor=ancestor->reference;
            }
            require(bool(ancestor),"Missing repeated reference.");
            PacketView repeated(*ancestor);repeated.pts=packet.pts;repeated.duration=packet.duration;
            return dispatchDecode(repeated,width,height,workers,deblocking,sharpening,destination,nullptr,presentation);
        }
        if(packet.data[4]==motionTemporalVersion){require(!deferred,"Motion coding is only valid at the outer frame level.");return decodeMotionTemporal(packet,width,height,workers,deblocking,destination,presentation);}
        auto expanded=prepareCoefficientTemporal(packet.data,packet.reference,workers,packet.keyIdentity);
        expanded.packet.pts=packet.pts;expanded.packet.duration=packet.duration;
        ScopedCoefficientBandOverrides override(expanded.coefficients.width?&expanded.coefficients:nullptr);
        return dispatchDecode(expanded.packet,width,height,workers,deblocking,sharpening,destination,nullptr,presentation);
    }
    case temporalPacketVersion:
        require(!deferred,"Temporal coding is only valid at the outer frame level.");
        return decodeTemporal(packet,width,height,workers,deblocking,sharpening,destination);
    case temporalCorrectionVersion:
        require(!deferred,"Temporal correction is only valid at the outer frame level.");
        return decodeTemporal(packet,width,height,workers,deblocking,sharpening,destination);
    case reconstruction441Version: {
        const auto view = read441(packet.data, width, height);
        DeferredUpscale native;
        native.shapes=view.settings.shapes;
        native.protectText=view.settings.protectText;
        native.allowGpuArtifact=!view.settings.protectText&&view.settings.intensity&&computeBackend()==ComputeBackend::Gpu&&uint64_t(width)*height<=4*1024*1024;
        auto frame = dispatchDecode({view.child, packet.pts, packet.duration}, width, height, workers, deblocking,
                                    sharpening, destination,&native);
        if(native.active){
            // A standalone AA/contrast control must not silently select a
            // different resampler at 1% and then lose its entire 1..100 range.
            const bool disabled=!view.settings.intensity;
            std::optional<FlatRegionLayout> flat;
            if(!native.flatMetadata.empty())flat=readFlatRegions(native.flatMetadata,width,height);
            const auto* protectedText=native.protectedText.width?&native.protectedText:nullptr;
            if(disabled&&protectedText)restoreDecodedText(frame.pixels,*protectedText,frame.width,frame.height);
            auto result=disabled?edgeUpscale(frame,width,height,workers,destination,std::min(1.f,native.contours/100.f),false,false)
                                :fastReconstruct441(frame,width,height,workers,view.budget,view.settings,destination,flat?&*flat:nullptr,protectedText,flat?nullptr:presentation,native.artifact.get());
            if(disabled&&(view.settings.antialias||view.settings.contrast))
                reconstruct441(destination.empty()?std::span(result.pixels):destination,width,height,workers,view.budget,view.settings);
            if(native.recycle)*native.recycle=std::move(frame.pixels);
            if(flat){
                applyFlatRegions(destination.empty()?std::span(result.pixels):destination,*flat,width,height,workers);
            }
            return result;
        }
        reconstruct441(destination.empty() ? std::span(frame.pixels) : destination, width, height, workers, view.budget,
                       view.settings,native.protectedText.width?&native.protectedText:nullptr);
        return frame;
    }
    case 129:
        return decodeBase(packet, width, height, workers, destination,deferred,deblocking,sharpening);
    case 77:
        return decodeFlatRegions(packet, width, height, workers, destination,deferred,deblocking,sharpening);
    case 28:
        return decodeScaled(packet, width, height, workers, destination,deferred,deblocking,sharpening);
    case 55:
        return decodeColorPrediction(packet, width, height, workers, destination,deblocking,sharpening);
    case 114:
        return decodeWavelet(packet, width, height, workers, destination, deblocking, sharpening,{},nullptr,false,{},32,false,0,deblocking);
    case 4:
        return decodeLossless(packet, width, height, destination);
    default:
        throw std::runtime_error("Unsupported retired packet format. Only current R441 components are supported.");
    }
}
} // namespace
Frame decodeFrameDeferred(PacketView packet,uint32_t width,uint32_t height,uint32_t workers,DeferredUpscale& deferred,std::span<uint8_t> destination){
    workers=safeWorkers(width,height,false,workers);ScopedWorkerBudget scope(workers);
    return dispatchDecode(packet,width,height,workers,100,0,destination,&deferred);
}
Frame decodeNestedFrame(PacketView packet,uint32_t width,uint32_t height,uint32_t workers,std::span<uint8_t> destination,DeferredUpscale* deferred,uint32_t deblocking,uint32_t sharpening){
    require(workers>0,"Missing validated worker budget.");
    return dispatchDecode(packet,width,height,workers,deblocking,sharpening,destination,deferred);
}
Frame decodeFrameWithSettings(PacketView packet, uint32_t width, uint32_t height, uint32_t threads, uint32_t deblocking,
                              uint32_t sharpening, bool,ComputeBackend backend) {
    ScopedComputeBackend compute(backend);
    threads=safeWorkers(width,height,false,threads);ScopedWorkerBudget scope(threads);
    require(sharpening<=200,"Invalid sharpening strength.");
    const bool fuse=packet.reference&&!packet.cachePresentation&&packet.data.size()>5&&packet.data[5]!=2&&
        (packet.data[4]==coefficientTemporalVersion||packet.data[4]==motionTemporalVersion);
    DecodePresentation presentation{fuse?sharpening:0};
    auto result=dispatchDecode(packet, width, height, threads, deblocking, 0, {},nullptr,&presentation);
    if(!presentation.applied){rememberTemporalKey(packet,result.pixels,width,height,threads,deblocking);finalSharpen(result.pixels,width,height,threads,sharpening);}return result;
}
void decodeFrameToBuffer(PacketView packet, uint32_t width, uint32_t height, uint32_t threads, uint32_t deblocking,
                         std::span<uint8_t> destination, uint32_t sharpening, bool,ComputeBackend backend) {
    ScopedComputeBackend compute(backend);
    require(destination.size() == pixelBytes(width, height), "Invalid decode destination.");
    threads=safeWorkers(width,height,false,threads);ScopedWorkerBudget scope(threads);
    require(sharpening<=200,"Invalid sharpening strength.");
    const bool fuse=packet.reference&&!packet.cachePresentation&&packet.data.size()>5&&packet.data[5]!=2&&
        (packet.data[4]==coefficientTemporalVersion||packet.data[4]==motionTemporalVersion);
    DecodePresentation presentation{fuse?sharpening:0};
    dispatchDecode(packet, width, height, threads, deblocking, 0, destination,nullptr,&presentation);
    if(!presentation.applied){rememberTemporalKey(packet,destination,width,height,threads,deblocking);finalSharpen(destination,width,height,threads,sharpening);}
}
Frame decodeFrameWithThreads(const Packet& packet, uint32_t width, uint32_t height, uint32_t threads) {
    return decodeFrameWithSettings(packet, width, height, threads, 100);
}
Frame decodeFrame(const Packet& packet, uint32_t width, uint32_t height) {
    return decodeFrameWithThreads(packet, width, height, 0);
}
} // namespace fajk
