#include "temporal_coefficients.hpp"
#include "fajk/entropy.hpp"
#include "fajk/hardware.hpp"
#include "spatial.hpp"
#include "timing.hpp"
#include "decode_session.hpp"
#include "coefficient_math.hpp"
#include <array>
#include <mutex>

namespace fajk {
namespace {
constexpr uint32_t magic=0x334b4a46;
constexpr uint8_t temporalVersion=coefficientTemporalVersion;
struct Layout {
    Bytes packet;
    size_t offset=0,size=0;
    // Every enclosing fixed-width child length grows by the native size delta.
    std::vector<size_t> lengths;
};
void locate(Layout& layout,size_t begin,size_t count,unsigned depth=0){
    require(depth<8&&count>=8&&begin<=layout.packet.size()&&count<=layout.packet.size()-begin,"Invalid coefficient wrapper.");
    ByteReader in{std::span(layout.packet).subspan(begin,count)};
    require(in.u32()==magic,"Invalid coefficient packet magic.");const auto version=in.u8();
    if(version==114){layout.offset=begin;layout.size=count;return;}
    size_t lengthOffset=0,childOffset=0,childSize=0;
    if(version==130){in.bytes(27);lengthOffset=in.pos;childSize=in.u32();childOffset=in.pos;in.bytes(childSize);in.end();}
    else if(version==129){in.bytes(19);lengthOffset=in.pos;childSize=in.u32();childOffset=in.pos;in.bytes(childSize);in.end();}
    else if(version==28){require(in.u8()==130&&in.u16()==0,"Unsupported coefficient scale.");in.bytes(16);lengthOffset=in.pos;childSize=in.u32();childOffset=in.pos;in.bytes(childSize);in.bytes(8);in.end();}
    else if(version==55){const auto tile=in.u8(),mode=in.u8();require((tile==32||tile==64)&&(mode==1||mode==5),"Unsupported coefficient color map.");in.bytes(in.var());childOffset=in.pos;childSize=count-childOffset;}
    else throw std::runtime_error("Unsupported coefficient wrapper.");
    if(lengthOffset)layout.lengths.push_back(begin+lengthOffset);
    locate(layout,begin+childOffset,childSize,depth+1);
}
Layout layout(const Packet& packet){
    require(packet.data.size()<=maxPacket&&packet.referenceIndex==UINT64_MAX&&!packet.reference,"Coefficient recoding requires an independent packet.");
    Layout result{packet.data};locate(result,0,result.packet.size());return result;
}
Bytes replace(const Layout& layout,std::span<const uint8_t> native){
    const int64_t difference=int64_t(native.size())-int64_t(layout.size);
    require(int64_t(layout.packet.size())+difference>0&&int64_t(layout.packet.size())+difference<=maxPacket,"Coefficient expansion exceeds packet limit.");
    Bytes result;result.reserve(size_t(int64_t(layout.packet.size())+difference));
    result.insert(result.end(),layout.packet.begin(),layout.packet.begin()+layout.offset);
    result.insert(result.end(),native.begin(),native.end());
    result.insert(result.end(),layout.packet.begin()+layout.offset+layout.size,layout.packet.end());
    for(auto offset:layout.lengths){ByteReader in{std::span(layout.packet).subspan(offset,4)};const auto old=in.u32();const int64_t size=int64_t(old)+difference;
        require(size>0&&size<=maxPacket,"Invalid coefficient child size.");for(unsigned b=0;b<4;++b)result[offset+b]=uint8_t(uint32_t(size)>>(8*b));}
    return result;
}
struct Band {
    uint32_t width=0,height=0;
    float step=1;
    bool prediction=false;
    Bytes coded;
    std::vector<float> integers;
};
struct Native {
    Bytes header;
    std::array<Bytes,17> streams;
    std::array<Band,54> bands;
    size_t retainedBytes()const noexcept{size_t result=header.capacity();for(const auto& stream:streams)result+=stream.capacity();for(const auto& band:bands)result+=band.coded.capacity()+band.integers.capacity()*sizeof(float);return result;}
};
void readNativeInto(const Layout& layout,uint32_t workers,bool decode,Native& result){
    TimingTimeline timing;
    ByteReader in{std::span(layout.packet).subspan(layout.offset,layout.size)};
    const auto header=in.bytes(32);result.header.assign(header.begin(),header.end());
    require(header[4]==114&&header[5]==3&&header[28]==0&&(header[29]==23||header[29]==31||header[29]==33)&&header[27]==0,"Unsupported coefficient precision/model.");
    ByteReader dimensions{header.subspan(8,10)};const auto width=dimensions.u32(),height=dimensions.u32();const auto fixed=dimensions.u16();pixelBytes(width,height);
    require(fixed>0&&header[18]>=100&&header[18]<=240&&header[19]>=64&&header[20]>=32&&header[26]<=4,"Invalid coefficient quantizer.");
    const auto w=(width+63)&~63u,h=(height+63)&~63u,cw=header[7]&66?((width+1)/2+63)&~63u:w,ch=header[7]&2?((height+1)/2+63)&~63u:h;
    require(uint64_t(w)*h+2*uint64_t(cw)*ch<=16*1024*1024,"Coefficient reference exceeds the 64 MiB cache budget.");
    std::array<std::span<const uint8_t>,17> coded;
    for(auto& stream:coded)stream=in.bytes(in.var());
    in.end();
    auto* captured=coefficientEncodeCapture;
    bool reuse=decode&&captured&&captured->valid&&captured->nativePacket.size()==layout.size&&
        std::equal(captured->nativePacket.begin(),captured->nativePacket.end(),layout.packet.begin()+layout.offset);
    // Independent entropy streams share the caller's validated worker budget,
    // just like the ordinary native decoder. Parse boundaries before dispatch.
    // Captured coefficient bands replace the spatial decoder, not these
    // entropy streams: metadata and the independent skeleton still need all
    // of them. Reuse must not silently serialize that remaining work.
    const auto entropyWorkers=layout.size>=32768?workers:1;
    parallelFor(17,entropyWorkers,[&](uint32_t i){if(coded[i].empty()){result.streams[i].clear();return;}
        size_t limit=0;
        if(!i)limit=34+size_t((width+7)/8)*((height+7)/8);
        else if(i==16)limit=size_t(width)*height;
        else{const auto pw=i>=6?cw:w,ph=i>=6?ch:h;const size_t area=size_t(pw)*ph;const auto kind=(i-1)%5;
            limit=kind==0?size_t(pw>>6)*(ph>>6)*5:kind==1?area+18:((area-area/4096)/3)*5;}
        result.streams[i]=entropyDecode(coded[i],limit);
    });
    timing.mark("decode.coefficients.native_entropy");
    for(unsigned c=0;c<3;++c)for(unsigned orientation=0;orientation<3;++orientation){
        ByteReader bandStream{result.streams[3+c*5+orientation]};
        for(unsigned level=0;level<6;++level){auto& band=result.bands[c*18+level*3+orientation];band.width=(c?cw:w)>>(level+1);band.height=(c?ch:h)>>(level+1);
            const auto coded=bandStream.bytes(bandStream.var());band.coded.assign(coded.begin(),coded.end());
            band.step=std::max(.35f,(fixed/16.f)*(orientation==2?header[19]/128.f:1.f)*std::pow(header[18]/256.f,float(level)))*(c?header[20]/128.f:1.f);
            band.prediction=(header[7]&16)&&level>=header[26];}
        bandStream.end();}
    timing.mark("decode.coefficients.native_layout");
    if(reuse)for(unsigned b=0;b<54;++b)reuse&=captured->bands[b].size()==size_t(result.bands[b].width)*result.bands[b].height;
    if(decode)parallelFor(54,workers,[&](uint32_t index){auto& band=result.bands[index];
        if(reuse){band.integers=captured->bands[index];return;}
        band.integers.assign(size_t(band.width)*band.height,0.f);
        if(!band.coded.empty())decodeSpatialBand(band.coded,band.integers.data(),band.width,band.width,band.height,1.f,false,band.prediction,false,false,true,true,nullptr,0,header[29]);});
    timing.mark("decode.coefficients.native_bands");
    if(reuse)captured->used=true;
}
Native readNative(const Layout& layout,uint32_t workers,bool decode=true){
    Native result;readNativeInto(layout,workers,decode,result);return result;
}
Bytes writeNative(const Native& native,bool emptyBands=false){
    auto streams=native.streams;
    for(unsigned c=0;c<3;++c)for(unsigned orientation=0;orientation<3;++orientation){ByteWriter out;
        for(unsigned level=0;level<6;++level){const auto& band=native.bands[c*18+level*3+orientation];out.var(emptyBands?0:uint32_t(band.coded.size()));if(!emptyBands)out.bytes(band.coded);}
        streams[3+c*5+orientation]=std::move(out.data);}
    ByteWriter out;out.bytes(native.header);
    for(const auto& stream:streams){const auto coded=stream.empty()?Bytes{}:entropyEncode(stream,true);out.var(uint32_t(coded.size()));out.bytes(coded);}
    return std::move(out.data);
}
}
std::optional<CoefficientTemporalResult> encodePreparedCoefficients(const Packet& current,const Layout& currentLayout,const Native& target,const Native& reference,uint32_t workers){
    for(unsigned b=0;b<54;++b)if(target.bands[b].width!=reference.bands[b].width||target.bands[b].height!=reference.bands[b].height)return std::nullopt;
    std::array<Bytes,54> coded;std::array<uint8_t,54> modes{};
    parallelFor(54,workers,[&](uint32_t b){const auto& band=target.bands[b];
        // An independent zero band already costs zero bytes. No residual can
        // be strictly smaller; retain mode 0 without copying/predicting all
        // its samples or running a spatial encoder whose result is discarded.
        if(band.coded.empty())return;
        const auto& old=reference.bands[b];const float ratio=old.step/band.step;
        std::vector<float> delta=band.integers;const bool bounded=coefficientPrediction(delta,old.integers,ratio,true);
        if(!bounded){coded[b]=band.coded;return;}
        auto residual=encodeSpatialBand(delta.data(),band.width,band.width,band.height,1.f,false,.5f,false,0,true,0,true,true,.02f,nullptr,0,nullptr,0,23);
        if(residual.size()<band.coded.size()){modes[b]=1;coded[b]=std::move(residual);}else coded[b]=band.coded;
    });
    const auto count=uint32_t(std::count(modes.begin(),modes.end(),uint8_t(1)));if(!count)return std::nullopt;
    const auto skeleton=replace(currentLayout,writeNative(target,true));
    ByteWriter out;out.u32(magic);out.u8(temporalVersion);out.u8(1);out.u16(0);out.u32(uint32_t(skeleton.size()));out.bytes(skeleton);
    for(unsigned b=0;b<54;++b){out.u8(modes[b]);out.var(uint32_t(coded[b].size()));out.bytes(coded[b]);}
    if(out.data.size()>=current.data.size())return std::nullopt;
    return CoefficientTemporalResult{{std::move(out.data),current.pts,current.duration},count};
}
std::optional<CoefficientTemporalResult> encodeCoefficients(const Packet& current,const Native& reference,uint32_t workers){
    try{const auto form=layout(current);const auto target=readNative(form,workers);return encodePreparedCoefficients(current,form,target,reference,workers);}
    catch(const std::runtime_error&){return std::nullopt;}
}
uint64_t coefficientReferenceScore(const Native& target,const Native& reference){
    uint64_t score=0;
    for(unsigned b=0;b<54;++b){const auto& own=target.bands[b];const auto& other=reference.bands[b];
        if(own.width!=other.width||own.height!=other.height)return UINT64_MAX;
        const auto count=std::min<size_t>(64,own.integers.size());uint64_t sum=0;
        const float ratio=other.step/own.step;
        for(size_t i=0;i<count;++i){const auto index=i*own.integers.size()/count;
            sum+=uint64_t(std::abs(own.integers[index]-predictCoefficient(other.integers[index],ratio)));}
        if(count)score+=sum*own.integers.size()/count;
    }
    return score;
}
Packet expandCoefficients(std::span<const uint8_t> data,const Native& reference,uint32_t workers){
    require(data.size()<=maxPacket,"Coefficient packet too large.");ByteReader in{data};require(in.u32()==magic&&in.u8()==temporalVersion&&in.u8()==1&&in.u16()==0,"Invalid coefficient temporal header.");
    Packet skeleton;const auto bytes=in.bytes(in.u32());skeleton.data.assign(bytes.begin(),bytes.end());auto targetLayout=layout(skeleton);auto target=readNative(targetLayout,workers,false);
    std::array<uint8_t,54> modes{};std::array<std::span<const uint8_t>,54> coded;
    for(unsigned b=0;b<54;++b){modes[b]=in.u8();require(modes[b]<=1,"Invalid coefficient temporal band mode.");coded[b]=in.bytes(in.var());}in.end();
    parallelFor(54,workers,[&](uint32_t b){auto& band=target.bands[b];const auto& old=reference.bands[b];require(band.width==old.width&&band.height==old.height,"Coefficient reference dimensions differ.");
        if(!modes[b]){band.coded.assign(coded[b].begin(),coded[b].end());return;}
        std::vector<float> values(size_t(band.width)*band.height);if(!coded[b].empty())decodeSpatialBand(coded[b],values.data(),band.width,band.width,band.height,1.f,false,false,false,false,true,true,nullptr,0,23);
        const float ratio=old.step/band.step;require(coefficientPrediction(values,old.integers,ratio),"Coefficient temporal overflow.");
        band.coded=encodeSpatialBand(values.data(),band.width,band.width,band.height,1.f,band.prediction,.5f,false,0,true,0,true,true,.02f,nullptr,0,nullptr,0,target.header[29]==33?33:23);
    });
    // Raw fallback bands may use short magnitude models (prefix 6/7).
    if(target.header[29]!=33)target.header[29]=31;
    auto restored=replace(targetLayout,writeNative(target));return {std::move(restored)};
}

std::optional<CoefficientEncodingInfo> coefficientEncodingInfo(const Packet& packet){
    try{
        const auto form=layout(packet);const auto native=std::span(form.packet).subspan(form.offset,form.size);
        require(native.size()>=32&&native[4]==114&&native[5]==3,"Unsupported native rate descriptor.");
        ByteReader in{native.subspan(8,10)};const auto width=in.u32(),height=in.u32();pixelBytes(width,height);const auto step=in.u16();require(step>0,"Invalid native rate descriptor.");
        return CoefficientEncodingInfo{width,height,step/16.f};
    }catch(const std::runtime_error&){return {};}
}
std::optional<CoefficientTemporalResult> encodeCoefficientTemporal(const Packet& current,const Packet& key,uint32_t workers){
    if(current.reference||key.reference||current.referenceIndex!=UINT64_MAX||key.referenceIndex!=UINT64_MAX)return std::nullopt;
    if(current.data==key.data){ByteWriter out;out.u32(magic);out.u8(temporalVersion);out.u8(2);out.u16(0);return CoefficientTemporalResult{{std::move(out.data),current.pts,current.duration},54};}
    try{return encodeCoefficients(current,readNative(layout(key),workers),workers);}catch(const std::runtime_error&){return std::nullopt;}
}
Packet expandCoefficientTemporal(std::span<const uint8_t> data,const Packet& key,uint32_t workers){
    if(data.size()==8&&data[5]==2){ByteReader in{data};require(in.u32()==magic&&in.u8()==temporalVersion&&in.u8()==2&&in.u16()==0,"Invalid coefficient repeat.");require(key.data.size()>=5&&!isTemporalPacketVersion(key.data[4])&&!key.reference&&key.referenceIndex==UINT64_MAX,"Repeat requires an independent reference.");return key;}
    return expandCoefficients(data,readNative(layout(key),workers),workers);
}
Packet expandCoefficientTemporal(std::span<const uint8_t> data,std::shared_ptr<const Packet> key,uint32_t workers){
    require(bool(key),"Missing coefficient reference.");
    if(data.size()==8&&data[5]==2)return expandCoefficientTemporal(data,*key,workers);
    struct Cache {std::weak_ptr<const Packet> packet;std::unique_ptr<Native> native;size_t retainedBytes()const noexcept{return native?native->retainedBytes():0;}};
    auto& cache=decodeState<Cache>();
    if(cache.packet.lock()!=key){auto decoded=std::make_unique<Native>(readNative(layout(*key),workers));cache.native=std::move(decoded);cache.packet=key;}
    return expandCoefficients(data,*cache.native,workers);
}
namespace {
struct CoefficientReference {Layout form;Native native;bool skeleton=false;size_t retainedBytes()const noexcept{return form.packet.capacity()+form.lengths.capacity()*sizeof(size_t)+native.retainedBytes();}};
struct CoefficientReferencePool {
    mutable std::mutex mutex;std::unique_ptr<CoefficientReference> idle;size_t limit=0;bool open=true;
    size_t retainedBytes()const noexcept{std::lock_guard lock(mutex);return idle?idle->retainedBytes():0;}
    void configure(size_t bytes){
        std::unique_ptr<CoefficientReference> discarded;
        {std::lock_guard lock(mutex);limit=bytes;if(idle&&idle->retainedBytes()>limit)discarded=std::move(idle);}
    }
    void close(){
        std::unique_ptr<CoefficientReference> discarded;
        {std::lock_guard lock(mutex);open=false;limit=0;discarded=std::move(idle);}
    }
    std::unique_ptr<CoefficientReference> take(){std::lock_guard lock(mutex);return std::move(idle);}
    void recycle(CoefficientReference* value){
        std::unique_ptr<CoefficientReference> discarded(value);
        {std::lock_guard lock(mutex);if(open&&limit&&value->retainedBytes()<=limit)idle.swap(discarded);}
    }
};
struct CoefficientReferenceCache {
    std::weak_ptr<const Packet> packet;std::shared_ptr<const CoefficientReference> value,root;
    std::shared_ptr<CoefficientReferencePool> pool=std::make_shared<CoefficientReferencePool>();
    ~CoefficientReferenceCache(){pool->close();}
    size_t retainedBytes()const noexcept{return (value?value->retainedBytes():0)+(root&&root!=value?root->retainedBytes():0)+pool->retainedBytes();}
    void remember(std::shared_ptr<const Packet> identity,const std::shared_ptr<const CoefficientReference>& current,bool independent=false){
        const size_t budget=activeDecodeSession?activeDecodeSession->cacheBudget():64u*1024u*1024u;
        const auto all=activeDecodeSession?activeDecodeSession->retainedBytes():0,own=retainedBytes();
        const auto others=all>own?all-own:0;
        const auto bytes=current->retainedBytes();auto nextRoot=independent?current:root;
        if(nextRoot&&nextRoot->retainedBytes()>64u*1024u*1024u)nextRoot.reset();
        const auto rootBytes=nextRoot&&nextRoot!=current?nextRoot->retainedBytes():0;
        const bool currentFits=others<=budget&&bytes<=budget-others;
        if(!currentFits||rootBytes>budget-others-bytes)nextRoot.reset();
        const auto keptRootBytes=nextRoot&&nextRoot!=current?nextRoot->retainedBytes():0;
        // An optional spare must fit beside every other active session cache.
        // Do not evict useful references merely to retain an idle allocation.
        pool->configure(currentFits?budget-others-bytes-keptRootBytes:0);
        root=std::move(nextRoot);packet=std::move(identity);value=current;
    }
    std::shared_ptr<CoefficientReference> acquire(){
        auto result=pool->take();if(!result)result=std::make_unique<CoefficientReference>();
        // Only the final shared-owner release returns storage to the pool.
        // Old weak handles expire before reuse and cannot resurrect a mutable
        // reference. The pool can safely outlive an evicted decoder session.
        return {result.release(),[owner=pool](CoefficientReference* value){owner->recycle(value);}};
    }
};
std::shared_ptr<const CoefficientReference> decodeCoefficientReference(std::span<const uint8_t> data,const CoefficientReference& reference,uint32_t workers){
    TimingTimeline timing;
    require(data.size()<=maxPacket,"Coefficient packet too large.");ByteReader in{data};require(in.u32()==magic&&in.u8()==temporalVersion&&in.u8()==1&&in.u16()==0,"Invalid coefficient temporal header.");
    Packet skeleton;const auto bytes=in.bytes(in.u32());skeleton.data.assign(bytes.begin(),bytes.end());
    auto result=decodeState<CoefficientReferenceCache>().acquire();result->form=layout(skeleton);readNativeInto(result->form,workers,false,result->native);auto& target=result->native;
    result->skeleton=std::all_of(target.bands.begin(),target.bands.end(),[](const Band& band){return band.coded.empty();});
    std::array<uint8_t,54> modes{};std::array<std::span<const uint8_t>,54> coded;
    for(unsigned b=0;b<54;++b){modes[b]=in.u8();require(modes[b]<=1,"Invalid coefficient temporal band mode.");coded[b]=in.bytes(in.var());}in.end();
    timing.mark("decode.coefficients.current_header");
    parallelFor(54,workers,[&](uint32_t b){auto& band=target.bands[b];const auto& old=reference.native.bands[b];require(band.width==old.width&&band.height==old.height,"Coefficient reference dimensions differ.");
        auto& values=band.integers;values.assign(size_t(band.width)*band.height,0.f);
        if(!coded[b].empty())decodeSpatialBand(coded[b],values.data(),band.width,band.width,band.height,1.f,false,modes[b]?false:band.prediction,false,false,true,true,nullptr,0,modes[b]?23:target.header[29]);
        if(modes[b]){const float ratio=old.step/band.step;require(coefficientPrediction(values,old.integers,ratio),"Coefficient temporal overflow.");}
        band.coded.clear();
    });
    timing.mark("decode.coefficients.current_bands");
    return result;
}
bool coefficientRepeat(std::span<const uint8_t> data){
    if(data.size()!=8||data[5]!=2)return false;ByteReader in{data};require(in.u32()==magic&&in.u8()==temporalVersion&&in.u8()==2&&in.u16()==0,"Invalid coefficient repeat.");return true;
}
std::shared_ptr<const CoefficientReference> resolveCoefficientReference(std::shared_ptr<const Packet> packet,uint32_t workers,uint32_t depth){
    auto& coefficientReferenceCache=decodeState<CoefficientReferenceCache>();
    require(packet&&packet->data.size()>=5&&depth<coefficientMaxDepth,"Missing or excessive coefficient reference chain.");
    if(coefficientReferenceCache.packet.lock()==packet)return coefficientReferenceCache.value;
    std::shared_ptr<const CoefficientReference> result;
    if(packet->data[4]==temporalVersion){
        const auto parent=resolveCoefficientReference(packet->reference,workers,depth+1);
        result=coefficientRepeat(packet->data)?parent:decodeCoefficientReference(packet->data,*parent,workers);
    }else{
        require(!packet->reference&&packet->referenceIndex==UINT64_MAX,"Coefficient chain requires an independent root.");
        // Root packet identities can be recreated at a bounded-chain restart
        // or seek. Reuse exact immutable payloads without another entropy pass.
        if(coefficientReferenceCache.root&&coefficientReferenceCache.root->form.packet==packet->data)result=coefficientReferenceCache.root;
        else{auto decoded=std::make_shared<CoefficientReference>();decoded->form=layout(*packet);decoded->native=readNative(decoded->form,workers);result=std::move(decoded);}
    }
    coefficientReferenceCache.remember(packet,result,packet->data[4]!=temporalVersion);return result;
}
}
CoefficientTemporalExpansion prepareCoefficientTemporal(std::span<const uint8_t> data,std::shared_ptr<const Packet> key,uint32_t workers,std::shared_ptr<const Packet> identity){
    auto& coefficientReferenceCache=decodeState<CoefficientReferenceCache>();
    TimingTimeline timing;
    require(bool(key),"Missing coefficient reference.");
    // An independent repeated packet can use its original decoder unchanged,
    // including formats for which coefficient prediction is not implemented.
    if(coefficientRepeat(data)&&key->data.size()>4&&!isTemporalPacketVersion(key->data[4])){
        CoefficientTemporalExpansion result;result.packet=expandCoefficientTemporal(data,*key,workers);return result;
    }
    // A motion frame may immediately refer to the coefficient frame just
    // displayed. Its expansion is already cached under that frame's identity;
    // resolving its parent first would unnecessarily decode the whole chain.
    // Check the identity's payload and parent as well: callers may construct
    // PacketViews themselves, and an identity must not change their pixels.
    const bool matchingIdentity=identity&&identity->reference==key&&
        identity->data.size()==data.size()&&std::equal(data.begin(),data.end(),identity->data.begin());
    std::shared_ptr<const CoefficientReference> current;
    if(matchingIdentity&&coefficientReferenceCache.packet.lock()==identity)
        current=coefficientReferenceCache.value;
    if(!current){
        const auto reference=resolveCoefficientReference(key,workers,0);
        timing.mark("decode.coefficients.reference");
        current=coefficientRepeat(data)?reference:decodeCoefficientReference(data,*reference,workers);
    }else timing.mark("decode.coefficients.reference");
    timing.mark("decode.coefficients.current");
    if(matchingIdentity)coefficientReferenceCache.remember(identity,current);
    CoefficientTemporalExpansion result;result.storage=current;
    // Dependent packets already carry this independent empty-band skeleton.
    // Re-encoding its unchanged entropy streams on every decode adds work;
    // the owned integer overrides below supply the actual coefficient bands.
    result.packet.data=current->skeleton?current->form.packet:replace(current->form,writeNative(current->native,true));
    timing.mark("decode.coefficients.skeleton");
    ByteReader dimensions{std::span(current->native.header).subspan(8,8)};result.coefficients.width=dimensions.u32();result.coefficients.height=dimensions.u32();
    for(unsigned b=0;b<54;++b){result.coefficients.active[b]=1;result.coefficients.integers[b]=current->native.bands[b].integers;}
    return result;
}
struct CoefficientTemporalEncoder::State {
    uint32_t workers=1,interval=120,depth=0,packetDepth=0;uint64_t keyIndex=UINT64_MAX,lastIndex=UINT64_MAX;int64_t keyPts=0;bool chains=false,reuseRoot=false,exhaustiveReferences=false;
    std::shared_ptr<const Packet> key,root;std::shared_ptr<const Native> native,rootNative;
};
CoefficientTemporalEncoder::CoefficientTemporalEncoder(uint32_t workers,uint32_t interval,bool chains,bool reuseRootAtDepth,bool exhaustiveReferences):state_(std::make_unique<State>()) {state_->workers=workers;state_->interval=interval;state_->chains=chains;state_->reuseRoot=reuseRootAtDepth;state_->exhaustiveReferences=exhaustiveReferences;}
CoefficientTemporalEncoder::~CoefficientTemporalEncoder()=default;
uint32_t CoefficientTemporalEncoder::encodedDepth() const{return state_->packetDepth;}
const Packet& CoefficientTemporalEncoder::preview(const Packet& current,uint64_t index){
    CoefficientTemporalEncoder trial(1,0);*trial.state_=*state_;
    auto packet=trial.encode(current,index);
    previewState_=std::move(trial.state_);previewPacket_=std::move(packet);return previewPacket_;
}
Packet CoefficientTemporalEncoder::commitPreview(){
    require(bool(previewState_),"Missing coefficient preview.");state_=std::move(previewState_);return std::move(previewPacket_);
}
void CoefficientTemporalEncoder::discardPreview(){previewState_.reset();previewPacket_={};}
Packet CoefficientTemporalEncoder::encode(Packet current,uint64_t index){
    discardPreview();
    auto& s=*state_;require(current.referenceIndex==UINT64_MAX&&!current.reference,"Expected independent coefficient input.");
    // Exact encoded repeats can refer to a fixed earlier picture indefinitely.
    // Their next *changed* image is refreshed if the five-second age expired.
    if(s.keyIndex!=UINT64_MAX&&current.data==s.key->data){auto result=encodeCoefficientTemporal(current,*s.key,s.workers)->packet;result.referenceIndex=s.chains?s.lastIndex:s.keyIndex;s.packetDepth=(s.chains?s.depth:0)+1;return result;}
    // Returning to the exact quantized key picture needs only a direct hold,
    // even when a different picture has been encoded in between. Reset the
    // predictor to that actual stored key, keeping seek depth and refresh age.
    if(s.chains&&s.keyIndex!=UINT64_MAX&&current.pts-s.keyPts<temporalMaxKeyDuration&&
       (!s.interval||index-s.keyIndex<s.interval)&&current.data==s.root->data){
        auto result=encodeCoefficientTemporal(current,*s.root,s.workers)->packet;result.referenceIndex=s.keyIndex;
        s.native=s.rootNative;
        s.key=s.root;s.lastIndex=s.keyIndex;s.depth=0;s.packetDepth=1;return result;
    }
    if(s.chains&&s.reuseRoot&&s.depth>=coefficientSeekDepth-1&&current.pts-s.keyPts<temporalMaxKeyDuration&&(!s.interval||index-s.keyIndex<s.interval)){
        s.native=s.rootNative;s.key=s.root;s.lastIndex=s.keyIndex;s.depth=0;
    }
    Layout currentLayout;std::unique_ptr<Native> parsed;
    try{currentLayout=layout(current);parsed=std::make_unique<Native>(readNative(currentLayout,s.workers));}catch(const std::runtime_error&){}
    const bool refresh=s.keyIndex==UINT64_MAX||current.pts-s.keyPts>=temporalMaxKeyDuration||
        (s.chains&&s.depth>=coefficientSeekDepth-1);
    if(!refresh){
        if(parsed&&s.native&&(!s.interval||index-s.keyIndex<s.interval)){
            auto candidate=encodePreparedCoefficients(current,currentLayout,*parsed,*s.native,s.workers);bool fromRoot=false;
            if(s.chains&&s.reuseRoot&&s.depth&&s.rootNative&&s.native!=s.rootNative){
                bool search=s.exhaustiveReferences;
                if(!search){const auto rootScore=coefficientReferenceScore(*parsed,*s.rootNative),recentScore=coefficientReferenceScore(*parsed,*s.native);
                    search=rootScore!=UINT64_MAX&&(recentScore==UINT64_MAX||rootScore<recentScore-recentScore/10);}
                if(search){auto root=encodePreparedCoefficients(current,currentLayout,*parsed,*s.rootNative,s.workers);
                    if(root&&(!candidate||root->packet.data.size()<=candidate->packet.data.size())){candidate=std::move(root);fromRoot=true;}}
            }
            if(candidate&&(s.chains||candidate->packet.data.size()*100<=current.data.size()*92)){
                candidate->packet.referenceIndex=s.chains&&!fromRoot?s.lastIndex:s.keyIndex;
                if(s.chains){
                    auto next=std::make_shared<Packet>(std::move(current));
                    s.native=std::move(parsed);s.key=std::move(next);s.lastIndex=index;s.depth=fromRoot?1:s.depth+1;
                }
                s.packetDepth=s.chains?s.depth:1;
                return std::move(candidate->packet);
            }
        }
    }
    auto next=std::make_shared<Packet>(current);
    s.native=std::move(parsed);s.rootNative=s.native;s.key=next;s.root=std::move(next);s.keyIndex=s.lastIndex=index;s.keyPts=current.pts;s.depth=s.packetDepth=0;return current;
}
}
