#pragma once
#include "fajk/types.hpp"
#include "fajk/hardware.hpp"
#include <atomic>

namespace fajk {
struct TemporalQualityMeasure {
    uint64_t rgb=0,chroma=0,gradient=0,lumaGradient=0,flicker=0;
};
struct TemporalQualityDecision {
    TemporalQualityMeasure candidate,independent;
    uint32_t damagedTiles=0;
    uint32_t hueTiles=0,rgbTiles=0,chromaTiles=0,gradientTiles=0,lumaGradientTiles=0;
    bool measuredFullFrame=true;
    bool accepted() const {
        // Max may trade a small amount of detail for less color noise and
        // flicker. Neutral luma noise also needs its own spatial guard: RGB
        // MSE and opponent-color gradients cannot distinguish a checkerboard
        // from a constant neutral offset of the same squared magnitude.
        return measuredFullFrame&&!damagedTiles&&candidate.rgb*100<=independent.rgb*103&&
            candidate.chroma<=independent.chroma&&candidate.gradient<=independent.gradient&&
            candidate.flicker<=independent.flicker;
    }
};
// Evaluate reconstructed pixels, never motion-search costs or quantizer size.
// Integer opponent-color error and its spatial gradient expose hue blocks and
// colored speckles even when a frame's aggregate RGB error looks reasonable.
// The same previous output is used for both alternatives' temporal errors.
inline TemporalQualityDecision compareTemporalQuality(const Frame& source,const Frame& candidate,
    const Frame& independent,uint32_t workers,const Frame* previousSource=nullptr,const Frame* previousOutput=nullptr,bool stopOnLocalArtifact=false,std::vector<uint32_t>* damagedTileIndices=nullptr){
    if(damagedTileIndices)damagedTileIndices->clear();
    source.validate();candidate.validate();independent.validate();
    require(candidate.width==source.width&&candidate.height==source.height&&
        independent.width==source.width&&independent.height==source.height,"Invalid temporal quality geometry.");
    const bool temporal=previousSource&&previousOutput;
    if(temporal){previousSource->validate();previousOutput->validate();require(previousSource->width==source.width&&previousSource->height==source.height&&
        previousOutput->width==source.width&&previousOutput->height==source.height,"Invalid temporal quality history.");}
    struct Error {int r,g,b,co,cg,luma;};
    auto error=[&](const Frame& image,size_t at){const int r=int(image.pixels[at])-source.pixels[at],g=int(image.pixels[at+1])-source.pixels[at+1],b=int(image.pixels[at+2])-source.pixels[at+2];return Error{r,g,b,r-b,2*g-r-b,r+2*g+b};};
    auto square=[](int64_t value){return uint64_t(value*value);};
    const auto columns=(source.width+31)/32,rows=(source.height+31)/32;
    std::vector<TemporalQualityDecision> tiles(size_t(columns)*rows);
    std::atomic<bool> stopped=false;
    parallelFor(rows,workers,[&](uint32_t ty){for(uint32_t tx=0;tx<columns;++tx){
        if(stopOnLocalArtifact&&stopped.load(std::memory_order_relaxed))return;
        auto& tile=tiles[size_t(ty)*columns+tx];int64_t ca=0,cb=0,ia=0,ib=0;uint32_t count=0;
        for(uint32_t y=ty*32;y<std::min(source.height,ty*32+32);++y)for(uint32_t x=tx*32;x<std::min(source.width,tx*32+32);++x){
            const auto at=(size_t(y)*source.width+x)*4;const auto c=error(candidate,at),i=error(independent,at);
            tile.candidate.rgb+=square(c.r)+square(c.g)+square(c.b);tile.independent.rgb+=square(i.r)+square(i.g)+square(i.b);
            tile.candidate.chroma+=4*square(c.co)+square(c.cg);tile.independent.chroma+=4*square(i.co)+square(i.cg);
            ca+=c.co;cb+=c.cg;ia+=i.co;ib+=i.cg;++count;
            for(auto offset:{x?size_t(4):size_t(0),y?size_t(source.width)*4:size_t(0)})if(offset){
                const auto nc=error(candidate,at-offset),ni=error(independent,at-offset);
                tile.candidate.gradient+=4*square(c.co-nc.co)+square(c.cg-nc.cg);
                tile.independent.gradient+=4*square(i.co-ni.co)+square(i.cg-ni.cg);
                tile.candidate.lumaGradient+=square(c.luma-nc.luma);
                tile.independent.lumaGradient+=square(i.luma-ni.luma);
            }
            if(temporal){unsigned change=0;for(unsigned channel=0;channel<3;++channel)change=std::max(change,unsigned(std::abs(int(source.pixels[at+channel])-previousSource->pixels[at+channel])));
                if(change<=3)for(unsigned channel=0;channel<3;++channel){const int signal=int(source.pixels[at+channel])-previousSource->pixels[at+channel];
                    tile.candidate.flicker+=square(int(candidate.pixels[at+channel])-previousOutput->pixels[at+channel]-signal);
                    tile.independent.flicker+=square(int(independent.pixels[at+channel])-previousOutput->pixels[at+channel]-signal);}}
        }
        // Ratios alone reject a cleaner predictor for a nearly clean tile's
        // tiny rounding error. Absolute floors distinguish those changes from
        // visible hue blocks/speckles. Global color/gradient/flicker guards
        // still disallow paying for a local repair with more overall noise.
        const bool hue=(std::abs(ca)>std::abs(ia)+4ll*count&&std::abs(ca)>16ll*count)||
            (std::abs(cb)>std::abs(ib)+8ll*count&&std::abs(cb)>32ll*count);
        tile.hueTiles=hue;
        tile.rgbTiles=tile.candidate.rgb>std::max(tile.independent.rgb*2+uint64_t(count)*12,uint64_t(count)*192);
        tile.chromaTiles=tile.candidate.chroma>std::max(tile.independent.chroma*2+uint64_t(count)*64,uint64_t(count)*512);
        tile.gradientTiles=tile.candidate.gradient>std::max(tile.independent.gradient*2+uint64_t(count)*64,uint64_t(count)*256);
        // Guard local neutral speckles and seams, rather than forcing every
        // frame's total luma gradient below intra: that global rule can reject
        // a cleaner temporal image over harmless moving-edge differences.
        // Luma is stored at 4x magnitude; byte-domain squared floors 4/16
        // therefore become 64/256 here. RGB/chroma/flicker guards still apply.
        tile.lumaGradientTiles=tile.candidate.lumaGradient>std::max(tile.independent.lumaGradient*2+uint64_t(count)*64,uint64_t(count)*256);
        tile.damagedTiles=tile.hueTiles||tile.rgbTiles||tile.chromaTiles||tile.gradientTiles||tile.lumaGradientTiles;
        if(stopOnLocalArtifact&&tile.damagedTiles)stopped.store(true,std::memory_order_relaxed);
    }});
    TemporalQualityDecision result;
    result.measuredFullFrame=!stopped.load(std::memory_order_relaxed);
    for(size_t index=0;index<tiles.size();++index){const auto& tile=tiles[index];
        if(damagedTileIndices&&tile.damagedTiles)damagedTileIndices->push_back(uint32_t(index));
        auto add=[](auto& a,const auto& b){a.rgb+=b.rgb;a.chroma+=b.chroma;a.gradient+=b.gradient;a.lumaGradient+=b.lumaGradient;a.flicker+=b.flicker;};
        add(result.candidate,tile.candidate);add(result.independent,tile.independent);result.damagedTiles+=tile.damagedTiles;
        result.hueTiles+=tile.hueTiles;result.rgbTiles+=tile.rgbTiles;result.chromaTiles+=tile.chromaTiles;result.gradientTiles+=tile.gradientTiles;
        result.lumaGradientTiles+=tile.lumaGradientTiles;
    }
    return result;
}
}
