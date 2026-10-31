#pragma once
#include "fajk/types.hpp"
#include "fajk/codec.hpp"
#include <optional>

namespace fajk {
constexpr uint8_t temporalPacketVersion=132;
constexpr uint32_t temporalTile=64;
constexpr uint8_t temporalCorrectionVersion=133;
constexpr uint32_t temporalCorrectionTile=16;
// Submission-thread state. All dependent frames reference a full keyframe,
// not another delta; codec workers can still encode independent keys in parallel.
class TemporalEncoder {
    std::shared_ptr<const Frame> anchor_,previous_,lastSource_;
    Packet previousPacket_;
    uint64_t anchorIndex_=UINT64_MAX;
    EncodeOptions options_;
    uint32_t interval_;
    uint32_t tolerance_,hold_;
    std::unique_ptr<FrameCodec> referenceCodec_;
    Frame reconstructedKey_;
    bool reconstructedKeyValid_=false;
    Packet committedKey_;
    uint64_t committedIndex_=UINT64_MAX;
    std::optional<Packet> encodeSimilar(std::shared_ptr<const Frame>,uint64_t);
public:
    TemporalEncoder(EncodeOptions options,uint32_t interval,uint32_t tolerance=0,uint32_t hold=10):options_(std::move(options)),interval_(interval),tolerance_(tolerance),hold_(hold){
        require(tolerance<=16&&hold>=1&&hold<=120&&(!tolerance||!options_.lossless),"Invalid approximate temporal options.");
    }
    std::optional<Packet> encode(std::shared_ptr<const Frame>,uint64_t index);
    void commitKey(const Packet&,uint64_t index);
    // Convenience for standalone callers; streaming uses shared ownership to
    // retain the source key without copying/reuploading the whole image.
    std::optional<Packet> encode(const Frame& frame,uint64_t index){return encode(std::make_shared<Frame>(frame),index);}
};
}
