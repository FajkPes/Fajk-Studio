#pragma once
#include "fajk/container.hpp"
#include "fajk/codec.hpp"
#include "spatial.hpp"
#include "fajk/entropy.hpp"
#include <ostream>
namespace fajk {
// Developer report: validate the complete packet, then compare equivalent
// integer bands. Output bytes exclude wrappers and container overhead.
inline void reportCoefficientCompression(const Path& path,std::ostream& out){
    SequenceReader sequence(path);const auto packet=sequence.read(0);
    const auto& info=sequence.info();auto codec=makeCodec(info.codec);
    codec->decode(packet,info.width,info.height);
    std::span<const uint8_t> bytes=packet.data;
    for(;;){ByteReader in{bytes};in.bytes(4);const auto version=in.u8();
        if(version==130){in.bytes(27);bytes=in.bytes(in.u32());}
        else if(version==129){in.bytes(19);bytes=in.bytes(in.u32());}
        else if(version==77){in.bytes(11);const auto metadata=in.u32(),length=in.u32();in.bytes(metadata);bytes=in.bytes(length);}
        else if(version==28){in.bytes(19);bytes=in.bytes(in.u32());}
        else if(version==55){in.bytes(2);in.bytes(in.var());bytes=in.bytes(in.data.size()-in.pos);}
        else {require(version==114,"Report requires a native wavelet frame.");break;}
    }
    require(bytes.size()>=8&&(bytes[7]&2),"Report requires 4:2:0 coefficients.");
    ByteReader in{bytes};in.bytes(8);const auto width=in.u32(),height=in.u32();
    in.bytes(10);const auto predictionLevel=in.u8();in.u8();const auto adaptive=in.u8(),model=in.u8();
    in.bytes(2);require(!adaptive&&(model==23||model==31),"Report requires uniform mode-23/31 coefficients.");
    size_t before=0,after=0;out<<"channel,level,orientation,before_bytes,after_bytes\n";
    for(int stream=0;stream<17;++stream){const auto encoded=in.bytes(in.var());
        if(stream==0||stream==16||(stream-1)%5<2||encoded.empty())continue;
        const int channel=(stream-1)/5,orientation=(stream-3)%5;
        const uint32_t w=channel?((width+1)/2+63)&~63u:(width+63)&~63u;
        const uint32_t h=channel?((height+1)/2+63)&~63u:(height+63)&~63u;
        const auto data=entropyDecode(encoded,size_t(w)*h*5);ByteReader bands{data};
        for(int level=0;level<6;++level){auto band=bands.bytes(bands.var());if(band.empty())continue;
            const int bw=(w>>level)/2,bh=(h>>level)/2;const bool prediction=level>=predictionLevel;
            auto compact=compactSpatialBand(band,bw,bh,prediction);
            std::vector<float> a(size_t(bw)*bh,0.f),b(a);
            decodeSpatialBand(band,a.data(),bw,bw,bh,1.f,false,prediction,false,false,true,true,nullptr,0,model);
            decodeSpatialBand(compact,b.data(),bw,bw,bh,1.f,false,prediction,false,false,true,true,nullptr,0,31);
            require(a==b,"Lossless coefficient recoding changed values.");
            before+=band.size();after+=compact.size();out<<channel<<','<<level<<','<<orientation<<','<<band.size()<<','<<compact.size()<<'\n';
        }bands.end();
    }in.end();out<<"total,,,"<<before<<','<<after<<"\n";
}
}
