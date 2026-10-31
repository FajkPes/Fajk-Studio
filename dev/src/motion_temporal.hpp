#pragma once
#include "fajk/types.hpp"
#include "decode_internal.hpp"
#include "reconstruction441.hpp"
#include <optional>
namespace fajk {
struct DecodePresentation;
struct CoefficientEncodeCapture;
inline constexpr uint8_t motionTemporalVersion=137;
// Canonical reference pixels are independent of display filters/backend. Never
// feed sharpened/upscaled presentation pixels back into motion prediction.
struct MotionReference {
    Frame image;
    Reconstruction441 settings;
    uint32_t budget=0,surfaces=0,contours=0;
    bool scaled=false;
    float residualStep=32;
};
// Optional independent CPU presentation reuses this canonical inverse. Its
// base-cleanup budget comes from the child packet, while motion presentation
// retains the established outer-budget semantics.
std::optional<MotionReference> readMotionRoot(const Packet&,uint32_t width,uint32_t height,uint32_t workers,Frame* independentDisplay=nullptr);
// Lossless RGB repairs in disjoint native 32x32 tiles, relative to the decoded
// motion result. Applied to the canonical reference before display filtering.
Bytes encodeMotionRepairs(const Frame& predicted,const Frame& independent,std::span<const uint32_t> tiles,uint32_t workers);
Frame renderMotionReference(const MotionReference&,int64_t pts,int64_t duration,uint32_t width,uint32_t height,uint32_t workers,uint32_t deblocking=100,std::span<uint8_t> destination={},DecodePresentation* presentation=nullptr,uint32_t baseCleanupBudget=0);
Frame decodeMotionTemporal(PacketView,uint32_t width,uint32_t height,uint32_t workers,uint32_t deblocking,std::span<uint8_t> destination,DecodePresentation* presentation=nullptr);
class MotionTemporalEncoder {
    struct State;std::unique_ptr<State> state_;
public:
    MotionTemporalEncoder(EncodeOptions,uint32_t interval,uint32_t tolerance,uint32_t hold,bool stabilizeQuantizer=false);
    ~MotionTemporalEncoder();
    Packet encode(Frame,uint64_t index);
    Packet encodePrepared(Frame,Packet independent,CoefficientEncodeCapture,uint64_t index);
};
}
