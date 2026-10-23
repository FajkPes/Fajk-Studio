#include "tangent_cleanup.hpp"
#include "text_guard.hpp"
#include "fajk/hardware.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <bit>
namespace fajk {
namespace {
std::vector<DecodedTextProtection::Range> textRanges(std::span<const uint8_t> decoded,uint32_t width,uint32_t height,uint32_t workers,bool earlyReferences=false){
    require(decoded.size()==pixelBytes(width,height),"Invalid text guard image.");
    constexpr uint32_t tile=16;const uint32_t cols=(width+tile-1)/tile,rows=(height+tile-1)/tile;std::vector<uint8_t> tiles(size_t(cols)*rows,0);std::vector<uint16_t> neutralCount(tiles.size());
    // Reject colored surroundings before the expensive multi-distance stroke
    // test. This computes the exact same neutral counts as the old final gate.
    parallelFor(rows,workers,[&](uint32_t ty){
        const auto byteMask=_mm256_set1_epi32(255);
        for(uint32_t y=ty*tile;y<std::min(height,(ty+1)*tile);++y)for(uint32_t x=0;x<width;x+=8){
            const auto* row=reinterpret_cast<const int*>(decoded.data()+size_t(y)*width*4);
            const auto own=x+8<=width?_mm256_loadu_si256(reinterpret_cast<const __m256i*>(row+x)):_mm256_i32gather_epi32(row,_mm256_min_epi32(_mm256_set1_epi32(width-1),_mm256_setr_epi32(x,x+1,x+2,x+3,x+4,x+5,x+6,x+7)),4);
            // Only the low byte of each 32-bit pixel is used after the RGB
            // min/max. Keep exact byte arithmetic; six float conversions per
            // vector are unnecessary for the neutral-background predicate.
            const auto g=_mm256_srli_epi32(own,8),b=_mm256_srli_epi32(own,16);
            const auto hi=_mm256_max_epu8(own,_mm256_max_epu8(g,b)),lo=_mm256_min_epu8(own,_mm256_min_epu8(g,b));
            const auto spread=_mm256_and_si256(_mm256_subs_epu8(hi,lo),byteMask);
            const auto neutral=_mm256_or_si256(_mm256_cmpgt_epi32(_mm256_set1_epi32(33),spread),_mm256_cmpgt_epi32(_mm256_set1_epi32(48),_mm256_and_si256(hi,byteMask)));
            neutralCount[size_t(ty)*cols+x/tile]+=uint16_t(std::popcount(uint32_t(_mm256_movemask_ps(_mm256_castsi256_ps(neutral)))&((1u<<std::min(8u,width-x))-1)));
        }
    });
    parallelFor(rows,workers,[&](uint32_t ty){
        struct Hits{uint32_t count=0,minX=UINT32_MAX,maxX=0,minY=UINT32_MAX,maxY=0;std::array<uint16_t,16> strokes{};};std::vector<Hits> hits(cols);
        std::vector<uint8_t> eligible(cols);
        for(uint32_t tx=0;tx<cols;++tx){uint32_t neutral=0,total=0;for(uint32_t yy=ty?ty-1:ty;yy<=std::min(rows-1,ty+1);++yy)for(uint32_t xx=tx?tx-1:tx;xx<=std::min(cols-1,tx+1);++xx){neutral+=neutralCount[size_t(yy)*cols+xx];total+=std::min(tile,width-xx*tile)*std::min(tile,height-yy*tile);}eligible[tx]=neutral*100>=total*80;}
        const auto zero=_mm256_setzero_ps(),absMask=_mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));auto abs=[&](__m256 v){return _mm256_and_ps(v,absMask);};
        auto channel=[](__m256i p,int c){return _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srlv_epi32(p,_mm256_set1_epi32(c*8)),_mm256_set1_epi32(255)));};auto luma=[&](__m256i p){return _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(channel(p,0),channel(p,2)),_mm256_mul_ps(channel(p,1),_mm256_set1_ps(2))),_mm256_set1_ps(.25f));};
        for(uint32_t y=ty*tile;y<std::min(height,(ty+1)*tile);++y)for(uint32_t x=0;x<width;x+=8){if(!eligible[x/tile])continue;const auto ix=_mm256_setr_epi32(x,x+1,x+2,x+3,x+4,x+5,x+6,x+7);auto load=[&](int dx,int dy){const auto* row=reinterpret_cast<const int*>(decoded.data()+size_t(std::clamp(int(y)+dy,0,int(height)-1))*width*4);if(int64_t(x)+dx>=0&&uint64_t(x)+8+dx<=width)return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row+int64_t(x)+dx));const auto xx=_mm256_min_epi32(_mm256_set1_epi32(width-1),_mm256_max_epi32(_mm256_setzero_si256(),_mm256_add_epi32(ix,_mm256_set1_epi32(dx))));return _mm256_i32gather_epi32(row,xx,4);};
            const auto own=load(0,0);const auto r=channel(own,0),g=channel(own,1),b=channel(own,2);const auto hi=_mm256_max_ps(r,_mm256_max_ps(g,b)),lo=_mm256_min_ps(r,_mm256_min_ps(g,b));auto active=_mm256_and_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_srli_epi32(own,24),_mm256_set1_epi32(255))),_mm256_and_ps(_mm256_cmp_ps(_mm256_sub_ps(hi,lo),_mm256_set1_ps(48),_CMP_LE_OQ),_mm256_cmp_ps(hi,_mm256_set1_ps(52),_CMP_GE_OQ)));if(!_mm256_movemask_ps(active))continue;
            const auto center=luma(own);auto variation=zero;for(int axis=0;axis<2;++axis)for(int sign:{-1,1})variation=_mm256_max_ps(variation,abs(_mm256_sub_ps(center,luma(load(axis?0:sign,axis?sign:0)))));active=_mm256_and_ps(active,_mm256_cmp_ps(variation,_mm256_set1_ps(6),_CMP_GE_OQ));if(!_mm256_movemask_ps(active))continue;
            auto stroke=zero;for(int axis=0;axis<2;++axis)for(int distance:{2,4}){const auto a=load(axis?0:distance,axis?distance:0),other=load(axis?0:-distance,axis?-distance:0);const auto left=luma(a),right=luma(other),da=_mm256_sub_ps(center,left),db=_mm256_sub_ps(center,right);auto match=_mm256_and_ps(_mm256_cmp_ps(_mm256_mul_ps(da,db),zero,_CMP_GT_OQ),_mm256_and_ps(_mm256_cmp_ps(_mm256_min_ps(abs(da),abs(db)),_mm256_set1_ps(16),_CMP_GE_OQ),_mm256_cmp_ps(abs(_mm256_sub_ps(left,right)),_mm256_set1_ps(24),_CMP_LE_OQ)));match=_mm256_and_ps(match,_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_srli_epi32(_mm256_and_si256(a,other),24),_mm256_set1_epi32(255))));stroke=_mm256_or_ps(stroke,match);}
            uint32_t mask=uint32_t(_mm256_movemask_ps(_mm256_and_ps(active,stroke)));mask&=(1u<<std::min(8u,width-x))-1;if(!mask)continue;auto& hit=hits[x/tile];hit.strokes[y%tile]|=uint16_t(mask<<(x%tile));hit.count+=std::popcount(mask);hit.minX=std::min(hit.minX,x+std::countr_zero(mask));hit.maxX=std::max(hit.maxX,x+31-std::countl_zero(mask));hit.minY=std::min(hit.minY,y);hit.maxY=std::max(hit.maxY,y);
        }
        for(uint32_t tx=0;tx<cols;++tx){const auto& hit=hits[tx];
            // A single long outline is not text. Glyph neighborhoods have
            // separated strokes on multiple scanlines, even after compression.
            uint32_t multiple=0;for(const auto mask:hit.strokes)multiple+=std::popcount(uint32_t(mask)&~(uint32_t(mask)<<1))>=2;
            if(hit.count>=4&&((hit.maxX-hit.minX>=1&&hit.maxY-hit.minY>=3)||(hit.maxX-hit.minX>=3&&hit.maxY-hit.minY>=1)||hit.maxX-hit.minX>=8||hit.maxY-hit.minY>=8))
                tiles[size_t(ty)*cols+tx]=uint8_t(1|(multiple>=2&&hit.maxY-hit.minY>=3?2:0));
        }
    });
    // Read the immutable tile mask. A two-pixel border keeps glyph fringes
    // with their strokes; copying is exact, including alpha and RGB.
    std::vector<DecodedTextProtection::Range> ranges;
    size_t weakCount=0;
    for(uint32_t pass=0;pass<(earlyReferences?2u:1u);++pass){
        uint32_t previousTop=~0u,previousBottom=~0u;
        std::vector<std::pair<uint32_t,uint32_t>> rowRanges;
        for(uint32_t y=0;y<height;++y){
            const uint32_t top=(y>2?y-2:0)/tile,bottom=std::min(height-1,y+2)/tile;
            // The immutable mask is identical for all rows with the same
            // vertical tile support. Reuse horizontal runs, preserving every
            // two-pixel fringe, range order and early/fallback distinction.
            if(top!=previousTop||bottom!=previousBottom){
                rowRanges.clear();uint32_t first=0,last=0;
                auto copy=[&]{if(last>first)rowRanges.emplace_back(first,last);};
                for(uint32_t x=0;x<cols;++x){bool keep=false;
                    for(uint32_t yy=top;yy<=bottom;++yy)keep|=(tiles[size_t(yy)*cols+x]&(1u<<pass))!=0;
                    if(!keep)continue;
                    const auto begin=x*tile>2?x*tile-2:0,end=std::min(width,(x+1)*tile+2);
                    if(begin>last){copy();first=begin;last=end;}else last=std::max(last,end);
                }copy();previousTop=top;previousBottom=bottom;
            }
            for(auto [first,last]:rowRanges)
                ranges.push_back({(size_t(y)*width+first)*4,size_t(last-first)*4,pass!=0});
        }
        if(!pass)weakCount=ranges.size();
    }
    if(earlyReferences&&ranges.size()>weakCount){
        // Early glyph intervals are a subset of the wider fallback mask.
        // Subtract them from that mask: each reference pixel is stored and
        // restored once, with its appropriate point in the decode pipeline.
        std::vector<DecodedTextProtection::Range> disjoint;disjoint.reserve(ranges.size()*2);
        size_t strong=weakCount;
        for(size_t i=0;i<weakCount;++i){const auto end=ranges[i].offset+ranges[i].count;size_t cursor=ranges[i].offset;
            while(strong<ranges.size()&&ranges[strong].offset+ranges[strong].count<=cursor)++strong;
            while(strong<ranges.size()&&ranges[strong].offset<end){
                const auto first=std::max(cursor,ranges[strong].offset),last=std::min(end,ranges[strong].offset+ranges[strong].count);
                if(first>cursor)disjoint.push_back({cursor,first-cursor,false});
                if(last>first)disjoint.push_back({first,last-first,true});cursor=last;
                if(ranges[strong].offset+ranges[strong].count<=end)++strong;else break;
            }
            if(cursor<end)disjoint.push_back({cursor,end-cursor,false});
        }
        return disjoint;
    }
    return ranges;
}
}
DecodedTextProtection captureDecodedText(std::span<const uint8_t> decoded,uint32_t width,uint32_t height,uint32_t workers){
    DecodedTextProtection result;result.width=width;result.height=height;
    result.ranges=textRanges(decoded,width,height,workers,true);
    size_t bytes=0;for(const auto& range:result.ranges)bytes+=range.count;
    result.pixels.resize(bytes);size_t at=0;
    for(const auto& range:result.ranges){std::memcpy(result.pixels.data()+at,decoded.data()+range.offset,range.count);at+=range.count;}
    return result;
}
namespace {
void validateTextProtection(const DecodedTextProtection& protection,size_t imageBytes,uint32_t width,uint32_t height){
    require(protection.width==width&&protection.height==height&&imageBytes==pixelBytes(width,height),"Invalid protected text dimensions.");
    size_t at=0,end=0;
    for(const auto& range:protection.ranges){
        require(range.offset>=end&&range.offset<=imageBytes&&range.count<=imageBytes-range.offset&&
                at<=protection.pixels.size()&&range.count<=protection.pixels.size()-at&&
                range.offset%4==0&&range.count%4==0,"Invalid protected text interval.");
        end=range.offset+range.count;at+=range.count;
    }
    require(at==protection.pixels.size(),"Invalid protected text storage.");
}
}
void refreshDecodedTextFallback(DecodedTextProtection& protection,std::span<const uint8_t> decoded,uint32_t width,uint32_t height){
    validateTextProtection(protection,decoded.size(),width,height);
    size_t at=0;for(const auto& range:protection.ranges){if(!range.early)std::memcpy(protection.pixels.data()+at,decoded.data()+range.offset,range.count);at+=range.count;}
}
void restoreDecodedText(std::span<uint8_t> candidate,const DecodedTextProtection& protection,uint32_t width,uint32_t height){
    // Validate the complete sparse reference before changing any output.
    validateTextProtection(protection,candidate.size(),width,height);
    size_t at=0;
    for(const auto& range:protection.ranges){
        std::memcpy(candidate.data()+range.offset,protection.pixels.data()+at,range.count);at+=range.count;
    }
}
void preserveDecodedText(std::span<uint8_t> candidate,std::span<const uint8_t> decoded,uint32_t width,uint32_t height,uint32_t workers){
    require(candidate.size()==pixelBytes(width,height),"Invalid text guard output.");
    for(const auto& range:textRanges(decoded,width,height,workers))std::memcpy(candidate.data()+range.offset,decoded.data()+range.offset,range.count);
}
}
