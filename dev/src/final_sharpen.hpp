#pragma once
#include "fajk/types.hpp"
#include <cstring>
namespace fajk {
// Display-only final pass. Always follows reconstruction, text/flat restoration
// and temporal composition; 0 is an allocation-free identity.
void finalSharpen(std::span<uint8_t>,uint32_t width,uint32_t height,uint32_t workers,uint32_t strength);
void finalSharpenRowsAvx2(std::span<const uint8_t>,std::span<uint8_t>,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
inline uint32_t finalSharpPixel(std::span<const uint8_t> source,uint32_t width,uint32_t height,uint32_t x,uint32_t y,uint32_t strength){
    auto load=[&](uint32_t xx,uint32_t yy){uint32_t value;std::memcpy(&value,source.data()+(size_t(yy)*width+xx)*4,4);return value;};
    const uint32_t center=load(x,y),up=load(x,y?y-1:0),down=load(x,std::min(y+1,height-1)),left=load(x?x-1:0,y),right=load(std::min(x+1,width-1),y);
    if(((center&up&down&left&right)>>24)!=255)return center;
    auto light=[](uint32_t p){return int(((p&255)+2*((p>>8)&255)+((p>>16)&255)+2)>>2);};
    const int own=light(center),a=light(up),b=light(down),c=light(left),d=light(right);
    const int low=std::min({own,a,b,c,d}),high=std::max({own,a,b,c,d});
    const int detail=own*4-((a+b)+(c+d));
    const uint32_t gain=(strength*384+50)/100,blend=(strength*256+50)/100;
    const int magnitude=(std::abs(detail)*int(gain)+512)>>10;
    const int limit=(std::min(24,std::max(1,(high-low+4)>>2))*int(blend)+128)>>8;
    int delta=std::clamp(detail<0?-magnitude:magnitude,-limit,limit);
    // Attenuate already abrupt edges/isolated extrema, retain the full gain
    // on broad soft transitions. Content-driven, never a byte-budget fade.
    const int attenuation=std::max(64,256-4*std::max(0,2*std::abs(detail)-(high-low)-4));
    const int adaptive=(std::abs(delta)*attenuation+128)>>8;
    delta=delta<0?-adaptive:adaptive;
    const int red=center&255,green=(center>>8)&255,blue=(center>>16)&255;
    delta=std::clamp(delta,-std::min({red,green,blue}),255-std::max({red,green,blue}));
    return (center&0xff000000u)|uint32_t(red+delta)|(uint32_t(green+delta)<<8)|(uint32_t(blue+delta)<<16);
}
}
