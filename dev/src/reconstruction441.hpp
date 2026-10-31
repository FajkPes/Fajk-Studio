#pragma once
#include "fajk/types.hpp"
namespace fajk {
inline constexpr uint8_t reconstruction441Version=130;
inline constexpr uint32_t reconstruction441HeaderBytes=36;
void validate441(const Reconstruction441&);
inline uint16_t shapeTuningWord(const Reconstruction441& s){
    return s.shapes==ContourTuning{}?0:uint16_t(s.shapes.tolerance|(s.shapes.minimumSize<<8));
}
struct Reconstruction441Packet {
    Reconstruction441 settings;
    uint32_t budget;
    std::span<const uint8_t> child;
};
struct DecodedTextProtection;
Reconstruction441Packet read441(std::span<const uint8_t>,uint32_t width,uint32_t height);
void update441(Packet&,const Reconstruction441&);
void reconstruct441(std::span<uint8_t>,uint32_t width,uint32_t height,uint32_t workers,uint32_t budget,const Reconstruction441&,const DecodedTextProtection* protectedText=nullptr);
}
