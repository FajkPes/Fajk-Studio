#include "temporal.hpp"
#include "fajk/entropy.hpp"
#include "fajk/hardware.hpp"
#include "temporal_compare.hpp"
#include "temporal_coefficients.hpp"
#include <cstring>

namespace fajk {
std::optional<Packet> TemporalEncoder::encode(std::shared_ptr<const Frame> source,uint64_t index){
    require(bool(source),"Missing temporal source.");const auto& frame=*source;
    frame.validate();
    const bool late=anchorIndex_!=UINT64_MAX&&frame.pts>=anchor_->pts&&frame.pts-anchor_->pts>=temporalMaxKeyDuration;
    const bool repeat=late&&lastSource_&&frame.width==lastSource_->width&&frame.height==lastSource_->height&&frame.pixels==lastSource_->pixels;
    lastSource_=source;
    auto key=[&]() -> std::optional<Packet> {
        anchor_=source;anchorIndex_=index;previous_.reset();previousPacket_={};reconstructedKeyValid_=false;return std::nullopt;
    };
    if(anchorIndex_==UINT64_MAX||anchor_->width!=frame.width||anchor_->height!=frame.height)return key();
    if(late&&!repeat&&frame.pixels!=anchor_->pixels)return key();
    if(tolerance_)return encodeSimilar(std::move(source),index);
    // A repeated delta keeps the same reference even after its nominal age.
    // Only a subsequent change can trigger the deferred keyframe refresh.
    if(previous_&&previous_->pixels==frame.pixels){
        auto packet=previousPacket_;packet.pts=frame.pts;packet.duration=frame.duration;return packet;
    }
    const uint32_t cols=(frame.width+temporalTile-1)/temporalTile,rows=(frame.height+temporalTile-1)/temporalTile;
    std::vector<uint32_t> changed;
    for(uint32_t ty=0;ty<rows;++ty)for(uint32_t tx=0;tx<cols;++tx){
        const uint32_t x=tx*temporalTile,y=ty*temporalTile,w=std::min(temporalTile,frame.width-x),h=std::min(temporalTile,frame.height-y);
        bool equal=true;
        for(uint32_t dy=0;dy<h&&equal;++dy){const size_t at=(size_t(y+dy)*frame.width+x)*4;
            equal=std::memcmp(frame.pixels.data()+at,anchor_->pixels.data()+at,size_t(w)*4)==0;}
        if(!equal){
            // Expensive noise/motion goes directly to the established encoder.
            changed.push_back(ty*cols+tx);
            if(changed.size()*5>size_t(cols)*rows*2)return key();
        }
    }
    uint32_t budget=options_.targetBytes;
    if(!budget&&options_.automaticTarget&&!options_.lossless)budget=qualityTargetBytes(frame.width,frame.height,options_.quality);
    if(!budget&&!options_.lossless)budget=qualityTargetBytes(frame.width,frame.height,options_.quality);
    const size_t cap=budget?size_t(budget)*85/100:maxPacket;
    ByteWriter out;
    out.u32(0x334b4a46);out.u8(temporalPacketVersion);out.u8(1);out.u16(0);
    out.u32(frame.width);out.u32(frame.height);out.u32(uint32_t(changed.size()));
    uint64_t changedPixels=0;
    for(auto tile:changed){
        const uint32_t x=tile%cols*temporalTile,y=tile/cols*temporalTile;
        const uint32_t w=std::min(temporalTile,frame.width-x),h=std::min(temporalTile,frame.height-y);
        const size_t area=size_t(w)*h;
        Bytes differences(area*8);
        for(uint32_t dy=0;dy<h;++dy)for(uint32_t dx=0;dx<w;++dx){
            const size_t sourceOffset=(size_t(y+dy)*frame.width+x+dx)*4,at=size_t(dy)*w+dx;
            bool pixelChanged=false;
            for(size_t c=0;c<4;++c){
                const int difference=int(frame.pixels[sourceOffset+c])-int(anchor_->pixels[sourceOffset+c]);
                pixelChanged|=difference!=0;
                const uint16_t value=uint16_t(difference+255);
                differences[c*area+at]=uint8_t(value);differences[(c+4)*area+at]=uint8_t(value>>8);
            }
            changedPixels+=pixelChanged;
        }
        const auto encoded=entropyEncode(differences);
        out.u32(tile);out.u32(uint32_t(encoded.size()));out.bytes(encoded);
        if(out.data.size()>cap)return key();
    }
    // This is an adaptive refresh interval, not a periodic full-image tax.
    // Exact differences in <=1% of pixels may keep the same key indefinitely;
    // the packet-size and widespread-change safeguards still apply.
    if(interval_&&index-anchorIndex_>=interval_&&changedPixels*100>uint64_t(frame.width)*frame.height)return key();
    if(out.data.size()>cap)return key();
    Packet packet{std::move(out.data),frame.pts,frame.duration};packet.referenceIndex=anchorIndex_;
    if(changed.empty()){previous_.reset();previousPacket_={};}
    else{previous_=std::move(source);previousPacket_=packet;}
    return packet;
}
std::optional<Packet> TemporalEncoder::encodeSimilar(std::shared_ptr<const Frame> source,uint64_t index){
    const auto& frame=*source;
    auto key=[&]() -> std::optional<Packet>{anchor_=source;anchorIndex_=index;previous_.reset();previousPacket_={};reconstructedKeyValid_=false;return std::nullopt;};
    const uint32_t cols=(frame.width+15)/16,rows=(frame.height+15)/16;
    std::vector<std::vector<uint32_t>> dirtyRows(rows);std::vector<uint8_t> approximateRows(rows);
    parallelFor(rows,options_.threads,[&](uint32_t ty){for(uint32_t tx=0;tx<cols;++tx){
        const uint32_t x=tx*16,y=ty*16,w=std::min(16u,frame.width-x),h=std::min(16u,frame.height-y);
        const auto at=(size_t(y)*frame.width+x)*4;
        const auto match=compareTemporalBlock(frame.pixels.data()+at,anchor_->pixels.data()+at,size_t(frame.width)*4,w,h,tolerance_);
        if(!match.similar)dirtyRows[ty].push_back(ty*cols+tx);else if(match.different)approximateRows[ty]=1;
    }});
    std::vector<uint32_t> dirty;bool approximate=false;
    for(uint32_t y=0;y<rows;++y){dirty.insert(dirty.end(),dirtyRows[y].begin(),dirtyRows[y].end());approximate|=approximateRows[y]!=0;}
    // An unchanged image can live indefinitely. Approximate holds have a hard
    // lifetime from their independent source key, without any reference chain.
    if(approximate&&index-anchorIndex_>hold_)return key();
    if(interval_&&index-anchorIndex_>=interval_&&!dirty.empty())return key();
    if(dirty.empty()){
        ByteWriter out;out.u32(0x334b4a46);out.u8(temporalPacketVersion);out.u8(1);out.u16(0);out.u32(frame.width);out.u32(frame.height);out.u32(0);
        Packet result{std::move(out.data),frame.pts,frame.duration};result.referenceIndex=anchorIndex_;return result;
    }
    // Strong or widespread motion uses a clean independent frame. Numerical
    // corrections are lossless; no independently lossy image tiles are pasted
    // into the reference, which caused visible grids and old-pose ghosts.
    if(dirty.size()*5>size_t(cols)*rows*2)return key();
    uint32_t budget=options_.targetBytes;if(!budget)budget=qualityTargetBytes(frame.width,frame.height,options_.quality);
    const size_t cap=size_t(budget)*95/100;if(cap<64)return key();
    // Bound speculative exact-correction work before decoding a lossy key.
    // Fast reserves an optimistic 128 bytes per changed 16x16 block. More
    // quality may try larger, unusually compressible sets. This is a speed
    // policy, not a lower bound on entropy; a rejected set becomes a full
    // original frame, with no approximate omission or quality compromise.
    if(fastEncode(options_.speed)&&dirty.size()>std::max<size_t>(32,cap/128))return key();
    require(committedIndex_==anchorIndex_&&!committedKey_.data.empty(),"Encoded temporal key was not committed.");
    if(!reconstructedKeyValid_){
        if(!referenceCodec_)referenceCodec_=makeCodec(CodecId::Fajk);
        referenceCodec_->setThreads(options_.threads);referenceCodec_->setComputeBackend(options_.backend);
        referenceCodec_->setDeblocking(100);referenceCodec_->setSharpening(0);
        reconstructedKey_=referenceCodec_->decode(committedKey_,anchor_->width,anchor_->height);reconstructedKeyValid_=true;
    }
    ByteWriter out;out.u32(0x334b4a46);out.u8(temporalCorrectionVersion);out.u8(4);out.u16(temporalCorrectionTile);
    out.u32(frame.width);out.u32(frame.height);out.u32(uint32_t(dirty.size()));
    size_t encodedTiles=0;
    for(auto tile:dirty){
        const uint32_t x=tile%cols*16,y=tile/cols*16,w=std::min(16u,frame.width-x),h=std::min(16u,frame.height-y);
        const size_t area=size_t(w)*h;Bytes differences(area*8);
        for(uint32_t dy=0;dy<h;++dy)for(uint32_t dx=0;dx<w;++dx){
            const size_t at=(size_t(y+dy)*frame.width+x+dx)*4,p=size_t(dy)*w+dx;
            for(size_t c=0;c<4;++c){const uint16_t d=uint16_t(int(frame.pixels[at+c])-int(reconstructedKey_.pixels[at+c])+255);
                differences[c*area+p]=uint8_t(d);differences[(c+4)*area+p]=uint8_t(d>>8);}
        }
        const auto encoded=entropyEncode(differences);out.u32(tile);out.u32(uint32_t(encoded.size()));out.bytes(encoded);
        if(out.data.size()>cap)return key();
        // Fast mode must not spend a whole frame's encode time discovering
        // that thousands of exact corrections will exceed its byte budget.
        // After eight real tile encodes, abandon a projected >2x overflow.
        // This only selects a clean independent source frame; it never drops
        // corrections, changes the source or weakens the similarity threshold.
        ++encodedTiles;
        if(fastEncode(options_.speed)&&encodedTiles==8&&
           uint64_t(out.data.size()-20)*dirty.size()>uint64_t(cap)*encodedTiles*2)return key();
    }
    Packet result{std::move(out.data),frame.pts,frame.duration};result.referenceIndex=anchorIndex_;return result;
}

void TemporalEncoder::commitKey(const Packet& packet,uint64_t index){
    if(!tolerance_)return;
    require(index==anchorIndex_&&packet.referenceIndex==UINT64_MAX,"Committed temporal key does not match its source.");
    committedKey_=packet;committedIndex_=index;
}
}
