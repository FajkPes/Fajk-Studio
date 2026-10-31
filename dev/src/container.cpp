#include "fajk/container.hpp"
#include "temporal_coefficients.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

namespace fajk {
namespace {
constexpr size_t headerSize = 128, entrySize = 40;
constexpr std::array<uint8_t,8> magic{'F','A','J','K','V','I','D',0};
bool stillImagePath(const Path& path){auto extension=path.extension().wstring();std::transform(extension.begin(),extension.end(),extension.begin(),::towlower);return extension==L".fajk";}
void put(Bytes& b, size_t p, uint64_t v, size_t n) {
    for (size_t i=0;i<n;++i) b.at(p+i)=uint8_t(v>>(i*8));
}
uint64_t get(std::span<const uint8_t> b, size_t p, size_t n) {
    require(p <= b.size() && n <= b.size()-p, "Truncated metadata.");
    uint64_t v=0; for(size_t i=0;i<n;++i) v |= uint64_t(b[p+i])<<(i*8); return v;
}
void write(std::ostream& f, std::span<const uint8_t> b) {
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    require(bool(f), "Cannot write sequence (disk full or access denied).");
}
Bytes read(std::istream& f, size_t n) {
    Bytes b(n); f.read(reinterpret_cast<char*>(b.data()), std::streamsize(n));
    require(bool(f), "Truncated sequence."); return b;
}
Bytes header(const SequenceInfo& i, uint64_t idx, uint32_t indexCrc) {
    Bytes b(headerSize); std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,i.temporalChains?3:i.temporal?2:1,4); put(b,12,headerSize,4); put(b,16,i.width,4); put(b,20,i.height,4);
    put(b,24,i.fpsNum,4); put(b,28,i.fpsDen,4); put(b,32,uint32_t(i.codec),4);
    put(b,36,i.sourceStep,4); put(b,40,i.count,8); put(b,48,idx,8);
    put(b,56,uint64_t(i.duration),8); put(b,64,indexCrc,4);
    put(b,68,crc32(std::span(b).first(68)),4); return b;
}
}
uint32_t crc32(std::span<const uint8_t> bytes) {
    static const auto table=[] {
        std::array<std::array<uint32_t,256>,8> t{};
        for(uint32_t i=0;i<256;++i) { uint32_t c=i; for(int j=0;j<8;++j) c=(c>>1)^(0xedb88320u & (0u-(c&1))); t[0][i]=c; }
        for(size_t n=1;n<t.size();++n)for(uint32_t i=0;i<256;++i){const auto c=t[n-1][i];t[n][i]=t[0][c&255]^(c>>8);}
        return t;
    }();
    // IEEE CRC-32, same polynomial/bytes as existing files. Eight independent
    // table lookups break the byte-at-a-time dependency chain. SSE4.2 CRC32
    // would use a different polynomial and cannot replace this checksum.
    uint32_t c=~0u;size_t at=0;
    for(;at+8<=bytes.size();at+=8){uint64_t block;std::memcpy(&block,bytes.data()+at,8);block^=c;
        c=table[7][block&255]^table[6][(block>>8)&255]^table[5][(block>>16)&255]^table[4][(block>>24)&255]^
          table[3][(block>>32)&255]^table[2][(block>>40)&255]^table[1][(block>>48)&255]^table[0][block>>56];}
    for(;at<bytes.size();++at)c=table[0][(c^bytes[at])&255]^(c>>8);
    return ~c;
}
SequenceWriter::SequenceWriter(const Path& destination, SequenceInfo info, bool replace)
    :destination_(destination),info_(info),replace_(replace),stillImage_(stillImagePath(destination)) {
    pixelBytes(info.width,info.height);
    require(info.fpsNum && info.fpsDen && info.sourceStep, "Invalid sequence timebase.");
    require(info.codec==CodecId::Fajk,"Unknown codec.");
    require(!stillImage_||!info.temporal,"Temporal coding requires a .fajkv video.");
    require(replace || !std::filesystem::exists(destination),"Output already exists.");
    static std::atomic_uint64_t counter{0};
    temporary_=destination;
    temporary_+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L"."+std::to_wstring(++counter)+L".partial";
    // CREATE_NEW prevents accidental reuse of someone else's temporary file.
    HANDLE handle=CreateFileW(temporary_.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Cannot create output file."); CloseHandle(handle);
    try {
        file_.open(temporary_,std::ios::binary|std::ios::in|std::ios::out);
        require(bool(file_),"Cannot open temporary output.");
        write(file_,Bytes(headerSize));
    } catch(...) { file_.close();std::error_code ec; std::filesystem::remove(temporary_,ec); throw; }
}
SequenceWriter::~SequenceWriter() {
    file_.close();
    if(!committed_) { std::error_code ec; std::filesystem::remove(temporary_,ec); }
}
void SequenceWriter::append(const Packet& p) {
    require(!committed_,"Sequence already finalized.");
    require(!stillImage_||index_.empty(),"A .fajk image contains exactly one frame; use .fajkv for video.");
    require(!p.data.empty() && p.data.size()<=maxPacket,"Invalid encoded frame size.");
    require(index_.size()<maxFrames,"Too many frames.");
    require(p.pts>=0 && p.duration>0 && p.pts<=INT64_MAX-p.duration,"Invalid frame timestamp.");
    require(index_.empty() || p.pts>=index_.back().pts+index_.back().duration,"Overlapping frame timestamps.");
    const bool dependent=p.referenceIndex!=UINT64_MAX;
    require(!dependent||(info_.temporal&&p.referenceIndex<index_.size()&&
            ((info_.temporalChains&&p.data.size()>4&&(p.data[4]==coefficientTemporalVersion||p.data[4]==137))||!index_[size_t(p.referenceIndex)].reference)),"Invalid keyframe reference.");
    const auto depth=dependent?uint32_t(index_[size_t(p.referenceIndex)].dependencyDepth)+1:0;
    require(depth<=coefficientMaxDepth,"Temporal reference chain exceeds seek bound.");
    require((p.data.size()>4&&isTemporalPacketVersion(p.data[4]))==dependent,"Temporal payload/reference mismatch.");
    index_.push_back({position_,p.data.size(),p.pts,p.duration,crc32(p.data),dependent?uint32_t(p.referenceIndex+1):0});
    index_.back().dependencyDepth=uint8_t(depth);
    write(file_,p.data); position_+=p.data.size();
}
void SequenceWriter::finish() {
    require(!committed_ && !index_.empty(),"Cannot finalize empty sequence.");
    Bytes index(index_.size()*entrySize);
    for(size_t n=0;n<index_.size();++n) {
        const auto& e=index_[n]; size_t p=n*entrySize;
        put(index,p,e.offset,8); put(index,p+8,e.size,8); put(index,p+16,uint64_t(e.pts),8);
        put(index,p+24,uint64_t(e.duration),8); put(index,p+32,e.checksum,4);
        put(index,p+36,e.reference,4);
    }
    const auto indexOffset=position_; write(file_,index); position_+=index.size();
    info_.count=index_.size(); info_.duration=index_.back().pts+index_.back().duration;
    file_.seekp(0); write(file_,header(info_,indexOffset,crc32(index))); file_.flush();
    require(bool(file_),"Failed to flush sequence."); file_.close();
    HANDLE h=CreateFileW(temporary_.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"Cannot flush output to disk.");
    const bool flushed=FlushFileBuffers(h)!=FALSE; CloseHandle(h); require(flushed,"Output flush failed.");
    require(MoveFileExW(temporary_.c_str(),destination_.c_str(),MOVEFILE_WRITE_THROUGH|(replace_?MOVEFILE_REPLACE_EXISTING:0))!=FALSE,
            "Cannot commit output. Existing destination was preserved.");
    committed_=true;
}
SequenceReader::SequenceReader(const Path& path):file_(path,std::ios::binary) {
    require(bool(file_),"Cannot open sequence.");
    file_.seekg(0,std::ios::end); const auto end=file_.tellg();
    require(end>=std::streamoff(headerSize),"Sequence is too short.");
    const uint64_t size=uint64_t(end); file_.seekg(0);
    auto h=fajk::read(file_,headerSize);
    require(std::equal(magic.begin(),magic.end(),h.begin()),"Not a Fajk image/video container. Legacy .fajk2 ZIP files need the original compressor.");
    const auto version=get(h,8,4);
    require((version==1||version==2||version==3) && get(h,12,4)==headerSize,"Unsupported sequence version.");
    info_.temporal=version>=2;info_.temporalChains=version==3;
    require(!stillImagePath(path)||!info_.temporal,"A .fajk image cannot use temporal coding.");
    require(get(h,68,4)==crc32(std::span(h).first(68)),"Damaged sequence header.");
    require(std::all_of(h.begin()+72,h.end(),[](uint8_t b){return b==0;}),"Unsupported header flags.");
    info_.width=uint32_t(get(h,16,4)); info_.height=uint32_t(get(h,20,4)); pixelBytes(info_.width,info_.height);
    info_.fpsNum=uint32_t(get(h,24,4)); info_.fpsDen=uint32_t(get(h,28,4)); info_.codec=CodecId(get(h,32,4));
    info_.sourceStep=uint32_t(get(h,36,4)); info_.count=get(h,40,8);
    const uint64_t off=get(h,48,8), duration=get(h,56,8);
    require(info_.fpsNum && info_.fpsDen && info_.sourceStep,"Invalid timebase.");
    require(info_.codec==CodecId::Fajk,"Unsupported frame codec.");
    require(info_.count && info_.count<=maxFrames,"Invalid frame count.");
    require(!stillImagePath(path)||info_.count==1,"A .fajk image must contain exactly one frame.");
    require(duration && duration<=INT64_MAX,"Invalid sequence duration."); info_.duration=int64_t(duration);
    require(off>=headerSize && off<=size && info_.count*entrySize==size-off,"Invalid sequence index bounds.");
    file_.seekg(std::streamoff(off)); auto index=fajk::read(file_,size_t(info_.count*entrySize));
    require(get(h,64,4)==crc32(index),"Damaged sequence index.");
    index_.reserve(size_t(info_.count)); uint64_t next=headerSize; int64_t lastEnd=0;
    for(size_t n=0;n<info_.count;++n) {
        const size_t p=n*entrySize;
        const uint64_t pts=get(index,p+16,8), dur=get(index,p+24,8);
        require(pts<=INT64_MAX && dur>0 && dur<=uint64_t(INT64_MAX)-pts,"Invalid timestamp.");
        IndexEntry e{get(index,p,8),get(index,p+8,8),int64_t(pts),int64_t(dur),uint32_t(get(index,p+32,4))};
        e.reference=uint32_t(get(index,p+36,4));
        require((info_.temporal||!e.reference)&&(!e.reference||(e.reference<=n&&(info_.temporalChains||!index_[e.reference-1].reference))),"Invalid temporal index reference.");
        e.dependencyDepth=e.reference?uint8_t(index_[e.reference-1].dependencyDepth+1):0;
        require(e.dependencyDepth<=coefficientMaxDepth,"Temporal reference chain exceeds seek bound.");
        require(e.offset==next && e.size && e.size<=maxPacket && e.size<=off-next,"Invalid frame bounds.");
        require(e.pts>=lastEnd,"Non-monotonic timestamps.");
        next+=e.size; lastEnd=e.pts+e.duration; index_.push_back(e);
    }
    require(next==off && lastEnd==info_.duration,"Inconsistent sequence metadata.");
}
Packet SequenceReader::read(uint64_t frame) {
    require(frame<index_.size(),"Frame index out of range."); const auto& e=index_[size_t(frame)];
    file_.clear(); file_.seekg(std::streamoff(e.offset)); auto data=fajk::read(file_,size_t(e.size));
    require(crc32(data)==e.checksum,"Damaged frame (checksum mismatch).");
    Packet packet{std::move(data),e.pts,e.duration};
    require((packet.data.size()>4&&isTemporalPacketVersion(packet.data[4]))==bool(e.reference),"Temporal payload/reference mismatch.");
    if(e.reference){
        const auto anchor=uint64_t(e.reference-1);
        const bool repeat=info_.temporalChains&&packet.data.size()==8&&(packet.data[4]==coefficientTemporalVersion||packet.data[4]==137)&&packet.data[5]==2;
        if(repeat&&cachedRepeatIndex_==anchor){packet.reference=cachedRepeatReference_;}
        else{
            if(cachedReferenceIndex_!=anchor)read(anchor);
            packet.reference=cachedReference_;
            if(repeat){cachedRepeatReference_=packet.reference;cachedRepeatIndex_=anchor;}
        }
        packet.referenceIndex=anchor;
    }
    if(info_.temporalChains){
        // Only a following 8-byte repeat needs a second presentation copy.
        // Ordinary moving video keeps the existing decoder/cache workload.
        packet.cachePresentation=frame+1<index_.size()&&index_[size_t(frame+1)].size==8&&index_[size_t(frame+1)].reference==frame+1;
        if(cachedReferenceIndex_!=frame){cachedReference_=std::make_shared<Packet>(packet);cachedReferenceIndex_=frame;}
        packet.keyIdentity=cachedReference_;
        // An explicit seek may recreate an anchor that an old hold cache
        // still names. Future holds must use the freshly displayed identity.
        if(cachedRepeatIndex_==frame)cachedRepeatReference_=cachedReference_;
    }else if(!e.reference&&info_.temporal){
        if(cachedReferenceIndex_!=frame){cachedReference_=std::make_shared<Packet>(packet);cachedReferenceIndex_=frame;}
        packet.keyIdentity=cachedReference_;
    }
    return packet;
}
uint64_t SequenceReader::frameAt(int64_t pts) const {
    auto it=std::upper_bound(index_.begin(),index_.end(),pts,[](int64_t t,const IndexEntry& e){return t<e.pts;});
    return it==index_.begin()?0:uint64_t(it-index_.begin()-1);
}
}
