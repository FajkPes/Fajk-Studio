#include "fajk/entropy.hpp"
#include <algorithm>
#include <numeric>
#include <cstring>
#include <cmath>
#include <queue>
#include <bit>
#include "rans_model.hpp"
namespace fajk {
namespace {constexpr uint32_t bits=12,scale=1<<bits,lower=1<<23;}
namespace {
const auto& modelLogs(){static const auto table=[] {std::array<double,4097> values{};for(size_t f=1;f<values.size();++f)values[f]=std::log2(double(f));return values;}();return table;}
// Allocate the remaining model slots where they reduce coding cost most.
void normalize(const std::array<uint32_t,256>& hist, size_t count,
               std::array<uint32_t,256>& freq, std::array<uint32_t,256>& cum){
    uint32_t sum=0;
    for(int s=0;s<256;++s){freq[s]=hist[s]?std::max(1u,uint32_t(uint64_t(hist[s])*scale/count)):0;sum+=freq[s];}
    using Item=std::pair<double,int>;
    const auto& logs=modelLogs();auto benefit=[&](int s){return hist[s]*(logs[freq[s]+1]-logs[freq[s]]);};
    if(sum<scale){std::priority_queue<Item> queue;for(int s=0;s<256;++s)if(freq[s])queue.emplace(benefit(s),s);
        while(sum<scale){auto s=queue.top().second;queue.pop();++freq[s];++sum;queue.emplace(benefit(s),s);}}
    else if(sum>scale){auto cost=[&](int s){return hist[s]*(logs[freq[s]]-logs[freq[s]-1]);};
        std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
        for(int s=0;s<256;++s)if(freq[s]>1)queue.emplace(cost(s),s);
        while(sum>scale){auto s=queue.top().second;queue.pop();--freq[s];--sum;if(freq[s]>1)queue.emplace(cost(s),s);}}
    uint32_t total=0;for(int s=0;s<256;++s){cum[s]=total;total+=freq[s];}
}
Bytes dictionaryEncode(std::span<const uint8_t> input){
    struct Entry {uint32_t position=0,generation=0;};
    thread_local std::array<Entry,65536> table{};thread_local uint32_t generation=0;
    if(++generation==0){for(auto& entry:table)entry.generation=0;generation=1;}
    ByteWriter out;
    size_t pos=0,anchor=0;
    auto hash=[&](size_t p){uint32_t v;std::memcpy(&v,input.data()+p,4);return (v*2654435761u)>>16;};
    auto length=[&](size_t n){while(n>=255){out.u8(255);n-=255;}out.u8(uint8_t(n));};
    while(pos+4<=input.size()){
        const uint32_t h=hash(pos),ref=table[h].generation==generation?table[h].position:UINT32_MAX;table[h]={uint32_t(pos),generation};
        if(ref==UINT32_MAX||pos-ref>65535||std::memcmp(input.data()+ref,input.data()+pos,4)!=0){++pos;continue;}
        size_t end=pos+4;
        while(end+8<=input.size()){uint64_t a,b;std::memcpy(&a,input.data()+end,8);std::memcpy(&b,input.data()+ref+end-pos,8);const auto different=a^b;if(different){end+=std::countr_zero(different)/8;break;}end+=8;}
        while(end<input.size()&&input[ref+end-pos]==input[end])++end;
        const size_t literals=pos-anchor,match=end-pos-4;out.u8(uint8_t((std::min<size_t>(15,literals)<<4)|std::min<size_t>(15,match)));
        if(literals>=15)length(literals-15);out.bytes(input.subspan(anchor,literals));out.u16(uint16_t(pos-ref));if(match>=15)length(match-15);
        pos=end;anchor=pos;
    }
    size_t literals=input.size()-anchor;out.u8(uint8_t(std::min<size_t>(15,literals)<<4));if(literals>=15)length(literals-15);out.bytes(input.subspan(anchor));return std::move(out.data);
}
Bytes dictionaryDecode(std::span<const uint8_t> input,size_t count){
    ByteReader in{input};Bytes out;out.reserve(count);
    auto length=[&](size_t n){if(n==15){while(true){auto b=in.u8();require(n<=count&&b<=count-n,"Dictionary length exceeds output.");n+=b;if(b!=255)break;}}return n;};
    while(in.pos<input.size()){
        const auto token=in.u8();const auto literals=length(token>>4);require(literals<=count-out.size(),"Dictionary literal overflow.");auto bytes=in.bytes(literals);out.insert(out.end(),bytes.begin(),bytes.end());
        if(in.pos==input.size()){require((token&15)==0,"Invalid final dictionary token.");break;}
        const auto offset=in.u16();require(offset>0&&offset<=out.size(),"Invalid dictionary offset.");auto match=length(token&15);require(match<=count-out.size()&&count-out.size()-match>=4,"Dictionary match overflow.");match+=4;
        const size_t start=out.size(),base=start-offset;out.resize(start+match);
        if(offset==1)std::memset(out.data()+start,out[base],match);
        else {size_t copied=0;while(copied<match){const size_t chunk=std::min<size_t>(offset+copied,match-copied);std::memcpy(out.data()+start+copied,out.data()+base,chunk);copied+=chunk;}}
    }
    require(out.size()==count,"Truncated dictionary output.");return out;
}
Bytes ransEncode(std::span<const uint8_t> input,bool compactModels=false) {
    require(input.size()<=UINT32_MAX,"Entropy input too large.");
    auto raw=[&]{ByteWriter out;out.u8(0);out.u32(uint32_t(input.size()));out.bytes(input);return std::move(out.data);};
    if(input.empty())return raw();
    std::array<uint32_t,256> hist{},freq{},cum{};
    for(auto v:input)++hist[v];
    if(!compactModels){
        // A normalized 12-bit model cannot beat the empirical entropy.
        // Allow 0.001 bit/symbol for integer-rANS rounding and a full 32
        // state bits per lane before ruling out a win over the raw stream.
        // This is a lower bound, not a heuristic based on "random-looking" data.
        const unsigned lanes=input.size()>=1024?4u:1u;
        double idealBits=0;unsigned present=0;
        for(auto count:hist)if(count){++present;idealBits+=count*std::log2(double(input.size())/count);}
        const double minimumBytes=7.+3.*present+4.*lanes+(idealBits-.001*input.size()-32.*lanes)/8.;
        if(minimumBytes>=input.size()+5.)return raw();
    }
    normalize(hist,input.size(),freq,cum);
    uint32_t total=0;uint16_t present=0;
    for(int s=0;s<256;++s){cum[s]=total;total+=freq[s];if(freq[s])++present;}
    const bool interleaved=input.size()>=1024;
    std::array<RansEncoderSymbol,256> symbols{};
    for(int s=0;s<256;++s)symbols[s]=RansEncoderSymbol(freq[s]?freq[s]:1,cum[s]);
    Bytes stack;stack.reserve(input.size()+4);std::array<uint32_t,4> states{lower,lower,lower,lower};
    for(size_t i=input.size();i-->0;){auto& state=states[interleaved?(i&3):0];const auto& symbol=symbols[input[i]];
        while(state>=symbol.threshold){stack.push_back(uint8_t(state));state>>=8;}
        state=symbol.advance(state);
    }
    ByteWriter out;out.u8(interleaved?3:1);out.u32(uint32_t(input.size()));out.u16(present);
    for(int s=0;s<256;++s)if(freq[s]){out.u8(uint8_t(s));out.u16(uint16_t(freq[s]));}
    if(compactModels){
        ByteWriter sparse,dense;sparse.u16(present);dense.u16(uint16_t(present|0x8000));
        for(int start=0;start<256;start+=8){uint8_t mask=0;for(int b=0;b<8;++b)if(freq[start+b])mask|=uint8_t(1u<<b);dense.u8(mask);}
        unsigned remaining=present;
        for(int s=0;s<256;++s)if(freq[s]){sparse.u8(uint8_t(s));if(--remaining){sparse.var(freq[s]-1);dense.var(freq[s]-1);}}
        const auto& table=sparse.data.size()<dense.data.size()?sparse.data:dense.data;
        if(table.size()<out.data.size()-5){out.data.resize(5);out.data[0]=interleaved?9:8;out.bytes(table);}
    }
    for(int i=0;i<(interleaved?4:1);++i)out.u32(states[i]);for(auto it=stack.rbegin();it!=stack.rend();++it)out.u8(*it);
    return out.data.size()<input.size()+5?std::move(out.data):raw();
}
}
Bytes entropyEncode(std::span<const uint8_t> input,bool compactModels) {
    require(input.size()<=UINT32_MAX,"Entropy input too large.");
    if(input.size()<64)return ransEncode(input,compactModels);
    if(std::all_of(input.begin()+1,input.end(),[v=input.front()](uint8_t b){return b==v;})){ByteWriter out;out.u8(4);out.u32(uint32_t(input.size()));out.u8(input.front());return std::move(out.data);}

    auto dictionary=dictionaryEncode(input);
    // Highly repetitive streams (notably opaque alpha and flat regions) are
    // already tiny after dictionary coding; avoid a full rANS pass over zeros.
    if(dictionary.size()<input.size()/64){auto encoded=ransEncode(dictionary,compactModels);ByteWriter result;result.u8(2);result.u32(uint32_t(input.size()));result.u32(uint32_t(dictionary.size()));result.bytes(encoded);return std::move(result.data);}
    auto direct=ransEncode(input,compactModels);
    if(dictionary.size()<input.size()){
        auto encoded=ransEncode(dictionary,compactModels);
        if(encoded.size()+9<direct.size()){ByteWriter result;result.u8(2);result.u32(uint32_t(input.size()));result.u32(uint32_t(dictionary.size()));result.bytes(encoded);direct=std::move(result.data);}
    }

    // Small wide-alphabet streams otherwise often fall back to raw bytes
    // because their 256-symbol frequency tables cost more than the payload.
    if(compactModels&&input.size()<=4096){
        std::array<uint32_t,16> lowHist{},highHist{};
        for(auto value:input){++lowHist[value&15];++highHist[value>>4];}
        double estimate=100;
        for(const auto& hist:{lowHist,highHist})for(auto count:hist)if(count)estimate+=count*std::log2(double(input.size())/count)/8;
        if(estimate<direct.size()){
            Bytes low(input.size()),high(input.size());for(size_t i=0;i<input.size();++i){low[i]=input[i]&15;high[i]=input[i]>>4;}
            auto a=ransEncode(low,true),b=ransEncode(high,true);
            if(a.size()+b.size()+13<direct.size()){ByteWriter out;out.u8(10);out.u32(uint32_t(input.size()));out.u32(uint32_t(a.size()));out.u32(uint32_t(b.size()));out.bytes(a);out.bytes(b);direct=std::move(out.data);}
        }
    }
    return direct;
}
Bytes entropyEncodePartitioned(std::span<const uint8_t> input,std::span<const uint32_t> ends){
    auto direct=entropyEncode(input);if(input.size()<4096||ends.size()<2)return direct;
    require(ends.size()<=64&&ends.back()==input.size(),"Invalid entropy partitions.");
    std::vector<Bytes> partitions;size_t begin=0;
    for(auto end:ends){require(end>=begin&&end<=input.size(),"Invalid entropy partition boundary.");partitions.push_back(entropyEncode(input.subspan(begin,end-begin)));begin=end;}
    return entropySelectPartitioned(input,ends,std::move(direct),partitions);
}
Bytes entropySelectPartitioned(std::span<const uint8_t> input,std::span<const uint32_t> ends,Bytes direct,std::span<const Bytes> partitions){
    if(input.size()<4096||ends.size()<2)return direct;
    require(ends.size()<=64&&ends.back()==input.size()&&partitions.size()==ends.size(),"Invalid entropy partitions.");
    size_t partitionBytes=6,begin=0;
    for(size_t i=0;i<ends.size();++i){const auto end=ends[i];require(end>=begin&&end<=input.size(),"Invalid entropy partition boundary.");partitionBytes+=4+partitions[i].size();begin=end;}
    if(partitionBytes<direct.size()){ByteWriter out;out.data.reserve(partitionBytes);out.u8(6);out.u32(uint32_t(input.size()));out.u8(uint8_t(ends.size()));for(const auto& coded:partitions){out.u32(uint32_t(coded.size()));out.bytes(coded);}direct=std::move(out.data);}
    // Differences are reversible and make repeated ramps easier to code.
    // Reject a candidate from its histogram before doing another entropy pass.
    Bytes residual(input.size());std::array<uint32_t,256> histogram{};uint8_t previous=0;
    for(size_t i=0;i<input.size();++i){residual[i]=uint8_t(input[i]-previous);previous=input[i];++histogram[residual[i]];}
    double estimated=32;uint32_t present=0;
    for(auto count:histogram)if(count){++present;estimated+=double(count)*std::log2(double(input.size())/count)/8;}
    estimated+=present*3;
    if(estimated<direct.size()*.98){auto coded=entropyEncode(residual);if(coded.size()+5<direct.size()){ByteWriter out;out.u8(7);out.u32(uint32_t(input.size()));out.bytes(coded);direct=std::move(out.data);}}
    return direct;
}
Bytes entropyDecode(std::span<const uint8_t> input,size_t maxOutput) {
    ByteReader r{input};auto mode=r.u8();auto count=r.u32();require(count<=maxOutput,"Entropy expansion limit exceeded.");
    if(mode==4){const auto value=r.u8();r.end();return Bytes(count,value);}
    if(mode==0){auto bytes=r.bytes(count);r.end();return Bytes(bytes.begin(),bytes.end());}
    if(mode==2){auto intermediate=r.u32();require(uint64_t(intermediate)<=uint64_t(count)+count/255+16,"Dictionary expansion limit exceeded.");auto payload=r.bytes(r.data.size()-r.pos);require(!payload.empty()&&(payload[0]<=1||payload[0]==3||payload[0]==8||payload[0]==9),"Nested dictionary stream is forbidden.");auto dictionary=entropyDecode(payload,intermediate);require(dictionary.size()==intermediate,"Dictionary size mismatch.");return dictionaryDecode(dictionary,count);}
    if(mode==7){require(count>=4096,"Invalid difference stream length.");auto coded=r.bytes(r.data.size()-r.pos);require(!coded.empty()&&coded[0]<=4,"Nested difference stream is forbidden.");auto result=entropyDecode(coded,count);require(result.size()==count,"Invalid difference stream output.");uint8_t previous=0;for(auto& value:result){value=uint8_t(value+previous);previous=value;}return result;}
    if(mode==6){auto partitions=r.u8();require(partitions>=2&&partitions<=64&&count>=1024,"Invalid entropy partition count.");Bytes result;result.reserve(count);
        for(uint32_t i=0;i<partitions;++i){auto coded=r.bytes(r.u32());require(!coded.empty()&&coded[0]<=4,"Nested contextual or partition streams are forbidden.");auto decoded=entropyDecode(coded,count-result.size());result.insert(result.end(),decoded.begin(),decoded.end());}
        require(result.size()==count,"Invalid entropy partition output.");r.end();return result;
    }
    if(mode==10){
        require(count>0&&count<=4096,"Invalid nibble stream size.");const auto a=r.u32(),b=r.u32();auto low=r.bytes(a),high=r.bytes(b);r.end();
        auto allowed=[](std::span<const uint8_t> coded){return !coded.empty()&&(coded[0]<=1||coded[0]==3||coded[0]==4||coded[0]==8||coded[0]==9);};
        require(allowed(low)&&allowed(high),"Nested nibble stream is forbidden.");auto result=entropyDecode(low,count),upper=entropyDecode(high,count);
        require(result.size()==count&&upper.size()==count,"Invalid nibble stream output.");
        for(size_t i=0;i<count;++i){require(result[i]<=15&&upper[i]<=15,"Invalid nibble value.");result[i]|=upper[i]<<4;}return result;
    }
    const bool compact=mode==8||mode==9;
    require((mode==1||mode==3||compact) && count>0,"Invalid entropy mode.");auto tableCode=r.u16();
    const bool dense=compact&&(tableCode&0x8000);const auto present=uint32_t(tableCode&uint16_t(compact?0x7fff:0xffff));require(present>0&&present<=256,"Invalid entropy table.");
    std::array<uint32_t,256> freq{};std::array<uint32_t,scale> lookup{};
    uint32_t total=0;
    std::array<uint8_t,256> symbols{};uint32_t symbolCount=0;
    if(dense){for(unsigned start=0;start<256;start+=8){const auto mask=r.u8();for(unsigned b=0;b<8;++b)if(mask&(1u<<b))symbols[symbolCount++]=uint8_t(start+b);}require(symbolCount==present,"Invalid dense entropy symbols.");}
    for(uint32_t i=0;i<present;++i){const auto s=dense?symbols[i]:r.u8();
        uint32_t f=0;if(compact){if(i+1==present)f=scale-total;else{const auto encoded=r.var();require(encoded<scale,"Invalid compact entropy frequency.");f=encoded+1;}}else f=r.u16();
        require(f&&!freq[s]&&f<=scale-total,"Invalid entropy frequencies.");freq[s]=f;total+=f;}
    require(total==scale,"Entropy frequencies do not sum to 4096.");total=0;
    for(int s=0;s<256;++s){if(freq[s])std::fill_n(lookup.begin()+total,freq[s],uint32_t(s)|((freq[s]-1)<<8)|(total<<20));total+=freq[s];}
    std::array<uint32_t,4> states{lower,lower,lower,lower};const int lanes=mode==3||mode==9?4:1;
    for(int i=0;i<lanes;++i){states[i]=r.u32();require(states[i]>=lower,"Invalid rANS state.");}Bytes result(count);
    auto symbol=[&](uint32_t& state){const auto slot=state&(scale-1),entry=lookup[slot];state=(((entry>>8)&4095)+1)*(state>>bits)+slot-(entry>>20);while(state<lower)state=(state<<8)|r.u8();return uint8_t(entry);};
    size_t i=0;if(lanes==4){for(;i+4<=count;i+=4){result[i]=symbol(states[0]);result[i+1]=symbol(states[1]);result[i+2]=symbol(states[2]);result[i+3]=symbol(states[3]);}}
    for(;i<count;++i)result[i]=symbol(states[lanes==4?(i&3):0]);
    for(int lane=0;lane<lanes;++lane)require(states[lane]==lower,"Invalid rANS terminal state.");r.end();return result;
}
}
