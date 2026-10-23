#include "flat_regions.hpp"
#include "decode_internal.hpp"
#include "codec_internal.hpp"
#include "fajk/entropy.hpp"
#include "fajk/hardware.hpp"
#include <cstring>
#include <unordered_map>
namespace fajk {
bool hasAvx2();
bool waveletOpaqueAvx2(const uint8_t*,size_t);
// Only source-verified uniform RGB cells may override reconstructed pixels.
// The map is entropy-coded; neighboring equal cells do not store repeated RGB.
Bytes flatRegions(const Frame& f,int tile,uint32_t threads){
 const int cols=(f.width+tile-1)/tile,rows=(f.height+tile-1)/tile;const size_t cells=size_t(cols)*rows;
 std::vector<uint32_t> colors(cells);Bytes flat(cells);
 parallelFor(uint32_t((cells+31)/32),threads,[&](uint32_t batch){for(size_t cell=size_t(batch)*32;cell<std::min(cells,size_t(batch+1)*32);++cell){
  int bx=int(cell%cols)*tile,by=int(cell/cols)*tile,ex=std::min(int(f.width),bx+tile),ey=std::min(int(f.height),by+tile);uint32_t color;std::memcpy(&color,f.pixels.data()+(size_t(by)*f.width+bx)*4,4);color|=0xff000000u;
  bool solid=true;for(int y=by;y<ey&&solid;++y)for(int x=bx;x<ex;++x){uint32_t value;std::memcpy(&value,f.pixels.data()+(size_t(y)*f.width+x)*4,4);if((value|0xff000000u)!=color){solid=false;break;}}
  if(solid){flat[cell]=1;colors[cell]=color;}
 }});
 std::unordered_map<uint32_t,size_t> histogram;for(size_t i=0;i<cells;++i)if(flat[i])++histogram[colors[i]];
 std::vector<std::pair<size_t,uint32_t>> ranked;for(auto [color,count]:histogram)if(count>=4)ranked.emplace_back(count,color);
 std::sort(ranked.begin(),ranked.end(),std::greater<>());if(ranked.size()>8)ranked.resize(8);
 size_t covered=0;for(auto [count,color]:ranked)covered+=count;if(covered*5<cells||ranked.empty())return {};
 ByteWriter out;out.u8(uint8_t(tile));out.u8(uint8_t(ranked.size()));for(auto [count,color]:ranked)out.u32(color);
 for(size_t i=0;i<cells;++i){uint8_t index=0;if(flat[i])for(size_t j=0;j<ranked.size();++j)if(colors[i]==ranked[j].second){index=uint8_t(j+1);break;}out.u8(index);}
 return std::move(out.data);
}
FlatRegionLayout readFlatRegions(std::span<const uint8_t> data,uint32_t width,uint32_t height){
 pixelBytes(width,height);ByteReader in{data};FlatRegionLayout layout;
 layout.tile=in.u8();const auto count=in.u8();
 require((layout.tile==8||layout.tile==16)&&count>0&&count<=8,"Invalid flat layout.");
 for(int i=1;i<=count;++i){layout.palette[i]=in.u32();require((layout.palette[i]>>24)==255,"Invalid flat color.");}
 layout.cols=(width+layout.tile-1)/layout.tile;
 layout.map=in.bytes(size_t(layout.cols)*((height+layout.tile-1)/layout.tile));in.end();
 for(auto index:layout.map)require(index<=count,"Invalid flat palette index.");return layout;
}
Bytes predictFlatRegions(std::span<const uint8_t> raw,uint32_t width,uint32_t height){
 const auto layout=readFlatRegions(raw,width,height);Bytes result(raw.begin(),raw.end());
 const size_t start=raw.size()-layout.map.size();
 for(size_t i=0;i<layout.map.size();++i){const int left=i%layout.cols?layout.map[i-1]:0,top=i>=layout.cols?layout.map[i-layout.cols]:0,corner=i>=layout.cols&&i%layout.cols?layout.map[i-layout.cols-1]:0;
  result[start+i]=uint8_t(layout.map[i]-medianPredict(left,top,corner));}
 return result;
}
void restoreFlatRegionPrediction(Bytes& raw,uint32_t width,uint32_t height){
 pixelBytes(width,height);require(raw.size()>=2,"Truncated predicted flat map.");const uint32_t tile=raw[0],count=raw[1];
 require((tile==8||tile==16)&&count>0&&count<=8,"Invalid predicted flat layout.");
 const uint32_t cols=(width+tile-1)/tile;const size_t cells=size_t(cols)*((height+tile-1)/tile),start=2+count*4;
 require(raw.size()==start+cells,"Invalid predicted flat map size.");
 for(size_t i=0;i<cells;++i){const int left=i%cols?raw[start+i-1]:0,top=i>=cols?raw[start+i-cols]:0,corner=i>=cols&&i%cols?raw[start+i-cols-1]:0;
  raw[start+i]=uint8_t(raw[start+i]+medianPredict(left,top,corner));require(raw[start+i]<=count,"Invalid predicted flat index.");}
}
Frame decodeFlatRegions(PacketView packet,uint32_t width,uint32_t height,uint32_t threads,std::span<uint8_t> destination,DeferredUpscale* deferred,uint32_t deblocking,uint32_t sharpening){
 pixelBytes(width,height);require(packet.data.size()<=maxPacket,"Packet exceeds size limit.");
 require(destination.empty()||destination.size()==pixelBytes(width,height),"Invalid flat destination.");
 ByteReader in{packet.data};require(in.u32()==frameMagic&&in.u8()==77,"Invalid flat wrapper.");const auto version=in.u8();require((version==1||version==2)&&in.u16()==0,"Invalid flat wrapper version.");
 require(in.u32()==width&&in.u32()==height,"Flat dimensions differ.");
 const auto mapBytes=in.u32(),childBytes=in.u32();
 auto metadata=entropyDecode(in.bytes(mapBytes),34+size_t((width+7)/8)*((height+7)/8));
 if(version==2)restoreFlatRegionPrediction(metadata,width,height);
 const auto layout=readFlatRegions(metadata,width,height);const auto encoded=in.bytes(childBytes);in.end();
 // Native photographic/UI packets and their spatial wrapper are supported.
 // Reject another flat wrapper so the source map cannot recurse.
 require(encoded.size()>=16&&(encoded[4]==55||encoded[4]==114||encoded[4]==28),"Invalid flat child version.");
 PacketView child{encoded,packet.pts,packet.duration};Frame frame;
  frame=decodeNestedFrame(child,width,height,threads,destination,deferred,deblocking,sharpening);
 if(deferred&&deferred->active){deferred->flatMetadata=std::move(metadata);return frame;}
 const auto pixels=destination.empty()?std::span<uint8_t>(frame.pixels):destination;
 if(hasAvx2())require(waveletOpaqueAvx2(pixels.data(),pixels.size()),"Flat child must be opaque.");
 else for(size_t i=3;i<pixels.size();i+=4)require(pixels[i]==255,"Flat child must be opaque.");
 applyFlatRegions(pixels,layout,width,height,threads);return frame;
}
void applyFlatRegions(std::span<uint8_t> pixels,const FlatRegionLayout& layout,uint32_t width,uint32_t height,uint32_t threads){
 parallelFor(height,threads,[&](uint32_t y){
  for(uint32_t bx=0;bx<width;bx+=layout.tile){
   const auto index=layout.map[size_t(y/layout.tile)*layout.cols+bx/layout.tile];if(!index)continue;
   const uint32_t n=std::min(layout.tile,width-bx);auto* dst=pixels.data()+(size_t(y)*width+bx)*4;
   const auto color=_mm_set1_epi32(int(layout.palette[index]));uint32_t x=0;
   for(;x+4<=n;x+=4)_mm_storeu_si128(reinterpret_cast<__m128i*>(dst+x*4),color);
   for(;x<n;++x)std::memcpy(dst+x*4,&layout.palette[index],4);
  }
 });
}
}

