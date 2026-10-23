#pragma once
#include "packet_view.hpp"
#include <array>
namespace fajk {
struct DeferredUpscale;
Bytes flatRegions(const Frame& frame,int tile,uint32_t threads);
Bytes predictFlatRegions(std::span<const uint8_t>,uint32_t width,uint32_t height);
void restoreFlatRegionPrediction(Bytes&,uint32_t width,uint32_t height);
struct FlatRegionLayout {
    uint32_t tile=16,cols=0;
    std::array<uint32_t,9> palette{};
    std::span<const uint8_t> map;
};
FlatRegionLayout readFlatRegions(std::span<const uint8_t>,uint32_t width,uint32_t height);
Frame decodeFlatRegions(PacketView ,uint32_t width,uint32_t height,uint32_t threads,
                        std::span<uint8_t> destination={},DeferredUpscale* deferred=nullptr,uint32_t deblocking=100,uint32_t sharpening=0);
void applyFlatRegions(std::span<uint8_t>,const FlatRegionLayout&,uint32_t width,uint32_t height,uint32_t threads);
}
