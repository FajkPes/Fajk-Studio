#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/pipeline.hpp"
#include "fajk/compute.hpp"
#include "../src/temporal_coefficients.hpp"
#include "../src/timing.hpp"
#include "../src/wavelet.hpp"
#include "../src/clip_rate_control.hpp"
#include "../src/coefficient_math.hpp"
#include "../src/decode_session.hpp"
#include <iostream>
#include <source_location>
#include <future>
#include <windows.h>
using namespace fajk;
namespace {
void check(bool value,std::source_location where=std::source_location::current()) {if(!value)throw std::runtime_error("Coefficient temporal assertion at "+std::to_string(where.line()));}
template<class F>void rejected(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
Frame fixture(){
    Frame frame{641,385,0,333333,Bytes(pixelBytes(641,385))};
    for(uint32_t y=0;y<frame.height;++y)for(uint32_t x=0;x<frame.width;++x){const auto at=(size_t(y)*frame.width+x)*4;
        frame.pixels[at]=uint8_t(90+int(30*std::sin(x*.029))+int(20*std::cos(y*.021)));
        frame.pixels[at+1]=uint8_t(120+int(30*std::sin((x+y)*.012)));
        frame.pixels[at+2]=uint8_t(130+int(30*std::cos(x*.032))+int(20*std::sin(y*.051)));
        frame.pixels[at+3]=255;
    }return frame;
}
}
int main(){try{
    // Independent libm oracle: half ties, saturation, both operations, tails
    // and overflow bounds must agree for every vector lane and scalar tail.
    if(hasAvx2()){
        for(size_t count:{1u,7u,8u,9u,63u,257u})for(float ratio:{.5f,1.f,2.f,.37129f,123.f})for(bool difference:{false,true}){
            std::vector<float> reference(count),values(count),expected(count);bool bounded=true;
            for(size_t i=0;i<count;++i){reference[i]=float(int(i*8191%2097153)-1048576);if(i%7==0)reference[i]=float(int(i%9)-4);
                values[i]=i%11==0?1048576.f:float(int(i*97%8193)-4096);
                const auto p=std::lround(std::clamp(reference[i]*ratio,-1048576.f,1048576.f));
                expected[i]=difference?values[i]-p:values[i]+p;bounded&=difference?std::abs(expected[i])<1048576:std::abs(expected[i])<=1048576;}
            check(coefficientPrediction(values,reference,ratio,difference)==bounded);
            check(std::memcmp(values.data(),expected.data(),count*sizeof(float))==0);
        }
        for(int width:{1,7,8,9,31})for(bool bias:{false,true}){
            const int height=3,stride=width+5;std::vector<float> values(size_t(width)*height),output(size_t(stride)*height+8,-77.f),expected=output;
            for(size_t i=0;i<values.size();++i){values[i]=i%5==0?-0.f:float(int(i%13)-6);if(i%17==0)values[i]=1048576.f;}
            for(int y=0;y<height;++y)for(int x=0;x<width;++x){const auto v=values[size_t(y)*width+x];expected[size_t(y)*stride+x]=v?(bias?v+std::copysign(.15f,v):v)*1.371f:0.f;}
            restoreCoefficientBandAvx2(values.data(),output.data(),stride,width,height,1.371f,bias);
            check(std::memcmp(output.data(),expected.data(),output.size()*sizeof(float))==0);
        }
    }
    const auto base=fixture();auto changed=base;changed.pts=base.duration;
    for(size_t i=0;i<changed.pixels.size();i+=4)for(unsigned c=0;c<3;++c)changed.pixels[i+c]+=2;
    for(uint32_t y=170;y<186;++y)for(uint32_t x=300;x<316;++x)changed.pixels[(size_t(y)*changed.width+x)*4]^=15;
    auto codec=makeCodec(CodecId::Fajk);codec->setThreads(4);
    EncodeOptions options;options.speed=EncodeSpeed::MaxCompression;options.threads=4;options.targetBytes=12000;
    const auto key=std::make_shared<Packet>(codec->encode(base,options));const auto intra=codec->encode(changed,options);
    auto candidate=encodeCoefficientTemporal(intra,*key,4);check(bool(candidate)&&candidate->packet.data[5]==1&&candidate->predictedBands>0&&candidate->packet.data.size()<intra.data.size());
    {const auto info=coefficientEncodingInfo(*key);check(bool(info)&&info->step>0);
        ClipRateControl rate;check(rate.begin(base,options).preferredStep==0);rate.finish(*key);
        const auto preference=rate.begin(base,options).preferredStep;check(preference>0&&preference<=info->step);
        auto cut=base;std::fill(cut.pixels.begin(),cut.pixels.end(),255);check(rate.begin(cut,options).preferredStep==0);
    }
    candidate->packet.reference=key;candidate->packet.referenceIndex=0;
    {
        DecodeSession owner;ScopedDecodeSession binding(owner);
        auto first=std::make_shared<Packet>(candidate->packet);
        const auto original=prepareCoefficientTemporal(first->data,key,4,first);
        auto recreatedRoot=std::make_shared<Packet>(*key),again=std::make_shared<Packet>(candidate->packet);again->reference=recreatedRoot;
        std::vector<TimingEvent> events;CoefficientTemporalExpansion reused;
        {TimingCapture capture(&events);reused=prepareCoefficientTemporal(again->data,recreatedRoot,4,again);}
        check(std::count_if(events.begin(),events.end(),[](const auto& event){return std::string_view(event.stage)=="decode.coefficients.native_entropy";})==1);
        for(unsigned b=0;b<54;++b)check(std::equal(original.coefficients.integers[b].begin(),original.coefficients.integers[b].end(),reused.coefficients.integers[b].begin()));
        owner.setCacheBudget(0);check(owner.retainedBytes()==0);
        for(unsigned b=0;b<54;++b)check(std::equal(original.coefficients.integers[b].begin(),original.coefficients.integers[b].end(),reused.coefficients.integers[b].begin()));
    }
    // Reusing a just-expanded coefficient frame must not decode its chain
    // again, and an unrelated or damaged payload must not hit that cache.
    {
        auto identity=std::make_shared<Packet>(candidate->packet);
        const auto firstExpansion=prepareCoefficientTemporal(identity->data,key,4,identity);
        const auto reused=prepareCoefficientTemporal(identity->data,key,4,identity);
        check(firstExpansion.storage&&firstExpansion.storage==reused.storage);
        auto damaged=identity->data;damaged[0]^=255;
        rejected([&]{prepareCoefficientTemporal(damaged,key,4,identity);});
        // A valid view with another parent must not be remembered under the
        // original immutable identity and poison its next expansion.
        auto alternateParent=std::make_shared<Packet>(intra);
        const auto unrelated=prepareCoefficientTemporal(identity->data,alternateParent,4,identity);
        const auto restored=prepareCoefficientTemporal(identity->data,key,4,identity);
        bool differs=false;
        for(unsigned b=0;b<54;++b){
            const auto original=firstExpansion.coefficients.integers[b];
            check(std::equal(original.begin(),original.end(),restored.coefficients.integers[b].begin()));
            differs|=!std::equal(original.begin(),original.end(),unrelated.coefficients.integers[b].begin());
        }
        check(differs);
        std::array<std::vector<float>,54> held;
        for(unsigned b=0;b<54;++b)held[b].assign(firstExpansion.coefficients.integers[b].begin(),firstExpansion.coefficients.integers[b].end());
        // A borrowed old expansion must survive reuse attempts, alternating
        // quantized references and explicit eviction of all session caches.
        DecodeSession owner;
        {
            ScopedDecodeSession binding(owner);
            const auto retained=prepareCoefficientTemporal(identity->data,key,4,identity);
            std::weak_ptr<const void> expired;
            auto alternate=std::make_shared<Packet>(intra);
            for(unsigned n=0;n<8;++n){auto other=std::make_shared<Packet>(candidate->packet);other->reference=n%2?key:alternate;
                const auto expanded=prepareCoefficientTemporal(other->data,other->reference,4,other);
                if(n==1)expired=expanded.storage;
                if(n>=3)check(expired.expired());
                for(unsigned b=0;b<54;++b)check(std::equal(held[b].begin(),held[b].end(),retained.coefficients.integers[b].begin(),retained.coefficients.integers[b].end()));
            }
            owner.setCacheBudget(0);check(owner.retainedBytes()==0);
            for(unsigned b=0;b<54;++b)check(std::equal(held[b].begin(),held[b].end(),retained.coefficients.integers[b].begin(),retained.coefficients.integers[b].end()));
        }
    }
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu})for(uint32_t workers:{1u,4u})for(uint32_t deblocking:{0u,100u})for(uint32_t sharpening:{0u,50u}){
        codec->setComputeBackend(backend);codec->setThreads(workers);codec->setDeblocking(deblocking);codec->setSharpening(sharpening);
        const auto expected=codec->decode(intra,base.width,base.height),actual=codec->decode(candidate->packet,base.width,base.height);
        check(expected.pixels==actual.pixels&&actual.pts==changed.pts&&actual.duration==changed.duration);
    }
    codec->setSharpening(0);codec->setDeblocking(100);codec->setThreads(4);
    // The key and current image may use different quantizers and budgets.
    auto expensive=options;expensive.targetBytes=24000;const auto fine=std::make_shared<Packet>(codec->encode(base,expensive));
    if(auto rescaled=encodeCoefficientTemporal(intra,*fine,4)){rescaled->packet.reference=fine;rescaled->packet.referenceIndex=0;
        check(codec->decode(intra,base.width,base.height).pixels==codec->decode(rescaled->packet,base.width,base.height).pixels);}
    const auto repeat=encodeCoefficientTemporal(*key,*key,1);check(repeat&&repeat->packet.data.size()==8);
    check(expandCoefficientTemporal(repeat->packet.data,*key,1).data==key->data);
    auto lossless=options;lossless.lossless=true;lossless.targetBytes=0;const auto exact=codec->encode(base,lossless);auto exactRepeat=encodeCoefficientTemporal(exact,exact,1);
    check(exactRepeat&&expandCoefficientTemporal(exactRepeat->packet.data,exact,1).data==exact.data);
    auto bad=candidate->packet;bad.reference.reset();rejected([&]{codec->decode(bad,base.width,base.height);});
    auto nested=std::make_shared<Packet>(candidate->packet);for(unsigned i=0;i<coefficientMaxDepth;++i){auto next=std::make_shared<Packet>(candidate->packet);next->reference=nested;nested=std::move(next);}bad=candidate->packet;bad.reference=nested;rejected([&]{codec->decode(bad,base.width,base.height);});
    // Independent frames, direct references, sequential and reverse access.
    const auto path=std::filesystem::temp_directory_path()/(L"fajk-coefficient-"+std::to_wstring(GetCurrentProcessId())+L".fajkv");
    {SequenceInfo info{base.width,base.height,30,1,CodecId::Fajk,1,0,0,true};SequenceWriter writer(path,info,true);writer.append(*key);writer.append(candidate->packet);
        auto p=repeat->packet;p.referenceIndex=0;p.pts=base.duration*2;writer.append(p);writer.finish();}
    {SequenceReader reader(path);for(uint64_t index:{2,1,0,1,2}){const auto p=reader.read(index);const auto expected=codec->decode(index==1?intra:*key,base.width,base.height);
        check(codec->decode(p,base.width,base.height).pixels==expected.pixels);}}
    std::filesystem::remove(path);
    // Truncated headers, body, model selectors and reference sizes are rejected.
    for(size_t end=0;end<candidate->packet.data.size();end+=std::max<size_t>(1,candidate->packet.data.size()/31)){
        bad=candidate->packet;bad.data.resize(end);rejected([&]{codec->decode(bad,base.width,base.height);});}
    for(size_t at:{0u,4u,5u,6u,7u,8u,9u,10u,11u}){bad=candidate->packet;bad.data[at]^=255;rejected([&]{codec->decode(bad,base.width,base.height);});}
    bad=candidate->packet;bad.data.push_back(0);rejected([&]{codec->decode(bad,base.width,base.height);});
    CoefficientTemporalEncoder encoder(4,1);auto first=encoder.encode(*key,0);check(first.referenceIndex==UINT64_MAX);
    {CoefficientTemporalEncoder alternatives(4,0,true);
        check(alternatives.preview(*key,0).referenceIndex==UINT64_MAX);alternatives.discardPreview();
        check(alternatives.encode(*key,0).referenceIndex==UINT64_MAX);
        const auto proposed=alternatives.preview(intra,1);alternatives.discardPreview();
        const auto again=alternatives.preview(intra,1);check(proposed.data==again.data&&proposed.referenceIndex==again.referenceIndex);
        const auto committed=alternatives.commitPreview();check(committed.data==proposed.data&&committed.referenceIndex==0);
        rejected([&]{alternatives.commitPreview();});
    }
    auto held=encoder.encode(*key,10000);check(held.data.size()==8&&held.referenceIndex==0);
    auto refresh=encoder.encode(intra,10001);check(refresh.referenceIndex==UINT64_MAX&&refresh.data==intra.data);
    {
        CoefficientTemporalEncoder returning(4,0,true);
        auto root=*key;root.pts=0;check(returning.encode(root,0).referenceIndex==UINT64_MAX);
        auto change=intra;change.pts=base.duration;check(returning.encode(change,1).referenceIndex==0);
        root.pts=base.duration*2;const auto back=returning.encode(root,2);
        check(back.data.size()==8&&back.referenceIndex==0&&returning.encodedDepth()==1);
        auto seekable=back;seekable.reference=key;check(codec->decode(seekable,base.width,base.height).pixels==codec->decode(*key,base.width,base.height).pixels);
        change.pts=temporalMaxKeyDuration+1;check(returning.encode(change,3).referenceIndex==UINT64_MAX);
    }
    {
        bool exercised=false;
        for(unsigned difference:{4u,8u,16u,32u}){
            auto local=base;local.pts=base.duration*2;
            for(uint32_t y=160;y<192;++y)for(uint32_t x=288;x<320;++x)local.pixels[(size_t(y)*base.width+x)*4]+=uint8_t(difference);
            const auto independent=codec->encode(local,options);
            auto rootCandidate=encodeCoefficientTemporal(independent,*key,4),recentCandidate=encodeCoefficientTemporal(independent,intra,4);
            if(!rootCandidate||!recentCandidate||rootCandidate->packet.data.size()<=8||rootCandidate->packet.data.size()>=recentCandidate->packet.data.size())continue;
            CoefficientTemporalEncoder choices(4,0,true,true,true);
            const auto a=choices.encode(*key,0),b=choices.encode(intra,1),c=choices.encode(independent,2);
            check(b.referenceIndex==0&&c.referenceIndex==0&&choices.encodedDepth()==1);
            check(c.data==rootCandidate->packet.data&&c.data.size()<recentCandidate->packet.data.size());
            {SequenceInfo info{base.width,base.height,30,1,CodecId::Fajk,1,0,0,true,true};SequenceWriter writer(path,info,true);writer.append(a);writer.append(b);writer.append(c);writer.finish();}
            const auto expected=codec->decode(independent,base.width,base.height);
            {SequenceReader reader(path);check(codec->decode(reader.read(2),base.width,base.height).pixels==expected.pixels);
                codec->decode(reader.read(0),base.width,base.height);check(codec->decode(reader.read(2),base.width,base.height).pixels==expected.pixels);}
            std::filesystem::remove(path);
            auto after=local;after.pts=temporalMaxKeyDuration+1;after.pixels[0]^=31;
            check(choices.encode(codec->encode(after,options),3).referenceIndex==UINT64_MAX);
            exercised=true;break;
        }
        check(exercised);
    }
    auto dependent=intra;dependent.reference=key;check(!encodeCoefficientTemporal(dependent,*key,1));
    // Bounded chains keep exact independent output even across several refreshes.
    std::vector<Packet> originals;
    {SequenceInfo info{base.width,base.height,30,1,CodecId::Fajk,1,0,0,true,true};SequenceWriter writer(path,info,true);CoefficientTemporalEncoder chained(4,0,true);
        for(uint64_t n=0;n<70;++n){auto f=base;f.pts=int64_t(n)*f.duration;
            for(size_t i=0;i<f.pixels.size();i+=4)for(unsigned c=0;c<3;++c)f.pixels[i+c]+=uint8_t(n%7);
            originals.push_back(codec->encode(f,options));writer.append(chained.encode(originals.back(),n));}writer.finish();}
    {SequenceReader reader(path);check(reader.info().temporalChains);uint32_t predicted=0;
        for(const auto& entry:reader.index()){check(entry.dependencyDepth<=coefficientMaxDepth);predicted+=entry.reference!=0;}check(predicted>30);
        for(uint64_t index:{69,0,31,32,33,63,64,1,68}){const auto p=reader.read(index);check(codec->decode(p,base.width,base.height).pixels==codec->decode(originals[index],base.width,base.height).pixels);}}
    std::filesystem::remove(path);
    // A long exact hold has a fixed direct reference, while its next change
    // obeys the five-second timestamp limit even at a low/VFR frame rate.
    CoefficientTemporalEncoder timed(4,0,true);auto zero=*key;zero.pts=0;check(timed.encode(zero,0).referenceIndex==UINT64_MAX);
    zero.pts=600'000'000;auto longHold=timed.encode(zero,1800);check(longHold.data.size()==8&&longHold.referenceIndex==0);
    auto resumed=intra;resumed.pts=zero.pts+333333;check(timed.encode(resumed,1801).referenceIndex==UINT64_MAX);
    CoefficientTemporalEncoder lowRate(4,0,true);
    for(uint64_t n=0;n<12;++n){auto f=base;f.pts=int64_t(n)*10'000'000;for(size_t i=0;i<f.pixels.size();i+=4)for(unsigned c=0;c<3;++c)f.pixels[i+c]+=uint8_t(n*3);
        const auto p=lowRate.encode(codec->encode(f,options),n);if(n%5==0)check(p.referenceIndex==UINT64_MAX);}
    {SequenceInfo info{base.width,base.height,30,1,CodecId::Fajk,1,0,0,true,true};SequenceWriter writer(path,info,true);writer.append(*key);writer.append(candidate->packet);
        for(uint32_t n=2;n<4;++n){auto p=repeat->packet;p.pts=base.duration*n;p.referenceIndex=1;writer.append(p);}writer.finish();}
    {SequenceReader reader(path);codec->setComputeBackend(ComputeBackend::Gpu);codec->setThreads(4);
        const auto held=reader.read(2);const auto expected=codec->decode(intra,base.width,base.height);check(codec->decode(held,base.width,base.height).pixels==expected.pixels);
        const auto before=gpuActivity();const auto next=reader.read(3);check(next.reference==held.reference);
        check(codec->decode(next,base.width,base.height).pixels==expected.pixels);const auto after=gpuActivity();
        check(after.transforms==before.transforms&&after.nativeDecodes==before.nativeDecodes);}
    // The first repeat after an already displayed coefficient delta must use
    // that presentation too, without another transform/native decode.
    for(auto backend:{ComputeBackend::Cpu,ComputeBackend::Gpu})for(uint32_t filtering:{0u,100u}){
        SequenceReader reader(path);codec->setComputeBackend(backend);codec->setDeblocking(filtering);codec->setSharpening(50);
        const auto expected=codec->decode(reader.read(1),base.width,base.height);
        const auto before=gpuActivity();std::vector<TimingEvent> events;Frame actual;
        {TimingCapture capture(&events);actual=codec->decode(reader.read(2),base.width,base.height);}const auto after=gpuActivity();
        check(actual.pixels==expected.pixels&&actual.pts==base.duration*2);
        check(std::none_of(events.begin(),events.end(),[](const auto& event){return std::string_view(event.stage).starts_with("decode.coefficients.");}));
        check(after.transforms==before.transforms&&after.nativeDecodes==before.nativeDecodes);
    }
    // Separate decoder owners keep their cache when interleaved on one
    // thread, and one owner can migrate to another thread between calls.
    {
        SequenceReader a(path),b(path);auto first=makeCodec(CodecId::Fajk),second=makeCodec(CodecId::Fajk);
        first->setThreads(4);second->setThreads(4);first->setSharpening(0);second->setSharpening(0);
        const auto expected=first->decode(a.read(1),base.width,base.height);
        second->decode(b.read(1),base.width,base.height);
        std::vector<TimingEvent> events;
        {TimingCapture capture(&events);check(first->decode(a.read(2),base.width,base.height).pixels==expected.pixels);}
        check(std::none_of(events.begin(),events.end(),[](const auto& event){return std::string_view(event.stage).starts_with("decode.coefficients.");}));
        const auto held=a.read(3);
        auto moved=std::async(std::launch::async,[&]{std::vector<TimingEvent> migrated;Frame frame;
            {TimingCapture capture(&migrated);frame=first->decode(held,base.width,base.height);}
            check(std::none_of(migrated.begin(),migrated.end(),[](const auto& event){return std::string_view(event.stage).starts_with("decode.coefficients.");}));return frame;});
        check(moved.get().pixels==expected.pixels);
        first->setCacheBudgetBytes(0);first->decode(a.read(1),base.width,base.height);events.clear();
        {TimingCapture capture(&events);check(first->decode(a.read(2),base.width,base.height).pixels==expected.pixels);}
        check(std::any_of(events.begin(),events.end(),[](const auto& event){return std::string_view(event.stage).starts_with("decode.coefficients.");}));
        first->setCacheBudgetBytes(256u*1024u*1024u);first->decode(a.read(1),base.width,base.height);events.clear();
        {TimingCapture capture(&events);check(first->decode(a.read(2),base.width,base.height).pixels==expected.pixels);}
        check(std::none_of(events.begin(),events.end(),[](const auto& event){return std::string_view(event.stage).starts_with("decode.coefficients.");}));
    }
    std::filesystem::remove(path);
    // This fixture is deliberately above the GPU threshold; the smaller
    // general-purpose fixture also exercises the CPU fallback in GPU mode.
    if(gpuInfo().available){
        Frame large{641,449,0,333333,Bytes(pixelBytes(641,449),255)};
        for(uint32_t y=0;y<large.height;++y)for(uint32_t x=0;x<large.width;++x)for(unsigned c=0;c<3;++c)
            large.pixels[(size_t(y)*large.width+x)*4+c]=uint8_t(70+(x/8+y/7+c*19)%100);
        auto next=large;next.pts+=next.duration;for(size_t at=0;at<next.pixels.size();at+=4)for(unsigned c=0;c<3;++c)++next.pixels[at+c];
        auto nativeOptions=options;nativeOptions.targetBytes=24000;
        const auto root=encodeWavelet(large,nativeOptions,false),independent=encodeWavelet(next,nativeOptions,false);
        auto delta=encodeCoefficientTemporal(independent,root,4);check(bool(delta));delta->packet.referenceIndex=0;
        {SequenceWriter writer(path,{large.width,large.height,30,1,CodecId::Fajk,1,0,0,true,true},true);
            writer.append(root);writer.append(delta->packet);auto held=repeat->packet;held.pts=large.duration*2;held.referenceIndex=1;writer.append(held);writer.finish();}
        SequenceReader reader(path);codec->setComputeBackend(ComputeBackend::Gpu);codec->setSharpening(0);codec->setDeblocking(100);
        const auto start=gpuActivity();const auto expected=codec->decode(reader.read(1),large.width,large.height);const auto before=gpuActivity();
        check(before.nativeDecodes>start.nativeDecodes);
        check(codec->decode(reader.read(2),large.width,large.height).pixels==expected.pixels);const auto after=gpuActivity();
        check(after.nativeDecodes==before.nativeDecodes&&after.transforms==before.transforms);
    }
    std::filesystem::remove(path);
    std::cout<<"Coefficient temporal PASS: exact CPU/GPU pixels, rescaled quantizers, settings/workers, lossless repeats, random seek, independent references, malformed packets, bounded refresh\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
