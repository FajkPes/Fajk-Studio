#pragma once
#include "packet_view.hpp"
namespace fajk {
// Opacity has already been checked by the outer encoder for this frame.
Packet encodeColorPrediction(const Frame&,const EncodeOptions&,bool opaque);
Frame decodeColorPrediction(PacketView ,uint32_t,uint32_t,uint32_t,std::span<uint8_t> = {},uint32_t = 100,uint32_t = 0);
}
