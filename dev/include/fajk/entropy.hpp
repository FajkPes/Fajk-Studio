#pragma once
#include "types.hpp"
#include <array>
namespace fajk {
// Independently bounded rANS streams, with a raw fallback for incompressible data.
Bytes entropyEncode(std::span<const uint8_t> input, bool compactModels=false);
Bytes entropyEncodePartitioned(std::span<const uint8_t> input,
                               std::span<const uint32_t> ends);
// Select among already encoded independent partitions and the shared model.
Bytes entropySelectPartitioned(std::span<const uint8_t> input,
                              std::span<const uint32_t> ends, Bytes direct,
                              std::span<const Bytes> partitions);
Bytes entropyDecode(std::span<const uint8_t> input, size_t maxOutput);
constexpr uint32_t encodedVarSize(uint32_t value){uint32_t bytes=1;while(value>=128){value>>=7;++bytes;}return bytes;}
struct ByteWriter {
    Bytes data;
    void u8(uint8_t v){data.push_back(v);}
    void u16(uint16_t v){u8(uint8_t(v));u8(uint8_t(v>>8));}
    void u32(uint32_t v){u16(uint16_t(v));u16(uint16_t(v>>16));}
    void var(uint32_t v){while(v>=128){u8(uint8_t(v)|128);v>>=7;}u8(uint8_t(v));}
    void signedVar(int v){var(v<0?uint32_t(-int64_t(v)*2-1):uint32_t(v)*2);}
    void bytes(std::span<const uint8_t> v){data.insert(data.end(),v.begin(),v.end());}
};
struct ByteReader {
    std::span<const uint8_t> data; size_t pos=0;
    uint8_t u8(){require(pos<data.size(),"Truncated codec stream.");return data[pos++];}
    uint16_t u16(){auto a=u8();return uint16_t(a|(uint16_t(u8())<<8));}
    uint32_t u32(){auto a=u16();return uint32_t(a)|(uint32_t(u16())<<16);}
    uint32_t var(){uint32_t v=0;for(int i=0;i<5;++i){const auto b=u8();require(i<4||b<16,"Invalid variable integer.");v|=uint32_t(b&127)<<(i*7);if(!(b&128))return v;}throw std::runtime_error("Invalid variable integer.");}
    int signedVar(){auto v=var();return v&1?-int(v>>1)-1:int(v>>1);}
    std::span<const uint8_t> bytes(size_t n){require(pos<=data.size()&&n<=data.size()-pos,"Truncated codec stream.");auto r=data.subspan(pos,n);pos+=n;return r;}
    void end(){require(pos==data.size(),"Trailing codec data.");}
};
}
