#pragma once
#include "fajk/types.hpp"
#include <cstring>
namespace fajk {
// Display-only final pass. Always follows reconstruction, text/flat restoration
// and temporal composition; 0 is an allocation-free identity.
void finalSharpen(std::span<uint8_t>,uint32_t width,uint32_t height,uint32_t workers,uint32_t strength);
void finalSharpenLumaRowAvx2(const uint8_t* source,uint8_t* output,uint32_t width);
void finalSharpenRowAvx2(const uint8_t* rgba,const uint8_t* row,const uint8_t* up,const uint8_t* down,uint8_t* output,uint32_t width,uint32_t strength);
inline uint32_t finalSharpLumaPixel(uint32_t center,int own,int a,int b,int c,int d,uint32_t strength){
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
inline uint32_t finalSharpRowPixel(const uint8_t* row,const uint8_t* north,const uint8_t* south,uint32_t width,uint32_t x,uint32_t strength){
    auto load=[](const uint8_t* p,uint32_t xx){uint32_t value;std::memcpy(&value,p+size_t(xx)*4,4);return value;};
    const uint32_t center=load(row,x),up=load(north,x),down=load(south,x),left=load(row,x?x-1:0),right=load(row,std::min(x+1,width-1));
    if(((center&up&down&left&right)>>24)!=255)return center;
    auto light=[](uint32_t p){return int(((p&255)+2*((p>>8)&255)+((p>>16)&255)+2)>>2);};
    return finalSharpLumaPixel(center,light(center),light(up),light(down),light(left),light(right),strength);
}
inline uint32_t finalSharpPixel(std::span<const uint8_t> source,uint32_t width,uint32_t height,uint32_t x,uint32_t y,uint32_t strength){
    return finalSharpRowPixel(source.data()+size_t(y)*width*4,source.data()+size_t(y?y-1:0)*width*4,
        source.data()+size_t(std::min(y+1,height-1))*width*4,width,x,strength);
}
}
