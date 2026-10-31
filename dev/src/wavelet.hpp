#pragma once
#include "packet_view.hpp"
#include "color_model_statistics.hpp"
#include <array>
namespace fajk {
struct WaveletDebugCapture;
Frame inspectWavelet(PacketView,uint32_t,uint32_t,uint32_t,WaveletDebugCapture&,std::span<const uint8_t> = {},uint32_t = 64);
// Converted source shared by the color predictor and the transform. It belongs
// to one encode call and is consumed only by a matching color conversion.
struct WaveletSourceColor {
    std::array<std::vector<float>,3> planes;
    uint32_t width=0,height=0;
    bool fast=false,knownOpaque=false;
    std::vector<ColorModelPatch> fittedPatches;
};
// Six-level, symmetric CDF 9/7, normalized to sqrt(2) DC gain per axis.
// Working dimensions are padded to multiples of 64, so every lifting split is even.
void waveletTransform(std::span<float> samples,int stride,int height,uint32_t threads,bool inverse,bool allowAvx2=true,bool allowRowAvx2=true,int firstLevel=0,int levelCount=6);
Packet encodeWavelet(const Frame& frame,const EncodeOptions& options,bool allowFlatRegions=true,std::span<const uint8_t> cflMap={},uint32_t cflTile=32,std::span<const float> cflMeans={},WaveletSourceColor* sourceColor=nullptr);
Frame decodeWavelet(PacketView packet,uint32_t width,uint32_t height,uint32_t threads,std::span<uint8_t> destination={},uint32_t smoothing=100,uint32_t sharpening=0,std::span<const uint8_t> predictor={},std::vector<float>* lumaOutput=nullptr,bool lumaOnly=false,std::span<const uint8_t> cflMap={},uint32_t cflTile=32,bool cflCentered=false,uint8_t chromaModel=0,uint32_t nativeFilterStrength=100);
}
