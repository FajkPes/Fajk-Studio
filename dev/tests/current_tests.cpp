#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/entropy.hpp"
#include "fajk/hardware.hpp"
#include "../src/decode_internal.hpp"
#include "../src/frame_time.hpp"
#include "../src/frame_pool.hpp"
#include "../src/perceptual_color.hpp"
#include "../src/guided_chroma.hpp"
#include "color_prediction_fit_tests.hpp"
#include <iostream>
#include <random>
#include <source_location>
#include <atomic>
#include <thread>
using namespace fajk;
namespace fajk {void waveletColumnRangeAvx2(float*,float*,int,int,int,int,int,bool);bool hasAvx2();uint32_t gammaByteRowAvx2(const float*,uint32_t,uint32_t*);void spatialRateCoupling8Avx2(const float*,int,const float*,double&);}
namespace {
void check(bool value, std::source_location at = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Current codec assertion at " + std::to_string(at.line()));
}
template <class F> void rejected(F&& f) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed);
}
Frame fixture(uint32_t w, uint32_t h, bool alpha = false) {
    Frame f{w, h, 17, 23, Bytes(pixelBytes(w, h))};
    std::mt19937 random(44);
    for (size_t i = 0; i < f.pixels.size(); i += 4) {
        for (int c = 0; c < 3; ++c)
            f.pixels[i + c] = uint8_t(random());
        f.pixels[i + 3] = alpha ? uint8_t(random()) : 255;
    }
    return f;
}
} // namespace
int main() {
    try {
        testInterpolatedChroma([](bool value){check(value);});
        if(hasAvx2()){
            std::mt19937 random(490);
            for(int height=1;height<=16;++height)for(int repeat=0;repeat<4;++repeat){
                float errors[256]{},correlations[5];
                for(int y=0;y<height;++y)for(int x=0;x<8;++x)errors[y*16+x]=float(int(random()%18001)-9000)/128.f;
                for(auto& v:correlations)v=float(int(random()%20001)-10000)/10000.f;
                double reference=.25,vector=reference;
                for(int y=0;y<height;++y)for(int x=0;x<8;++x){
                    double neighbor=0;
                    if(x+1<8)neighbor+=correlations[0]*errors[y*16+x+1];
                    if(x+2<8)neighbor+=correlations[2]*errors[y*16+x+2];
                    if(y+1<height){
                        neighbor+=correlations[1]*errors[(y+1)*16+x];
                        if(x)neighbor+=correlations[4]*errors[(y+1)*16+x-1];
                        if(x+1<8)neighbor+=correlations[4]*errors[(y+1)*16+x+1];
                    }
                    if(y+2<height)neighbor+=correlations[3]*errors[(y+2)*16+x];
                    reference+=2*errors[y*16+x]*neighbor;
                }
                spatialRateCoupling8Avx2(errors,height,correlations,vector);check(vector==reference);
            }
        }
        {
            const char text[]="123456789";check(crc32({reinterpret_cast<const uint8_t*>(text),9})==0xcbf43926u);
            std::mt19937 random(472);Bytes bytes(65536+16);for(auto& value:bytes)value=uint8_t(random());
            for(size_t offset=0;offset<16;++offset)for(size_t count:{0u,1u,7u,8u,9u,15u,16u,63u,64u,65u,1024u,4097u,65536u}){
                const auto data=std::span<const uint8_t>(bytes).subspan(offset,count);uint32_t reference=~0u;
                for(uint8_t byte:data){reference^=byte;for(int bit=0;bit<8;++bit)reference=(reference>>1)^(0xedb88320u&(0u-(reference&1)));}
                check(crc32(data)==~reference);
            }
        }
        if(hasAvx2()){
            std::mt19937 random(462);
            for(uint32_t width:{1u,2u,7u,8u,9u,17u,64u,129u}){
                const uint32_t colors=(width+1)/2;
                std::array<std::vector<float>,6> rows;
                std::vector<float> luma(width),u(width),v(width),referenceU(width),referenceV(width);
                for(auto& row:rows){row.resize(colors);for(auto& value:row)value=float(int(random()%2049)-1024)*.125f;}
                for(auto& value:luma)value=float(int(random()%2049)-1024)*.125f;
                for(uint8_t model:{0,1,2,3})for(float guide:{4.f,8.f,32.f}){
                    nativeGuidedChromaRow(luma.data(),rows[0].data(),rows[1].data(),rows[2].data(),rows[3].data(),rows[4].data(),rows[5].data(),int(colors)-1,width,1/(guide*guide),u.data(),v.data(),true,model);
                    nativeGuidedChromaRow(luma.data(),rows[0].data(),rows[1].data(),rows[2].data(),rows[3].data(),rows[4].data(),rows[5].data(),int(colors)-1,width,1/(guide*guide),referenceU.data(),referenceV.data(),false,model);
                    for(uint32_t x=0;x<width;++x){check(std::abs(u[x]-referenceU[x])<.0002f);check(std::abs(v[x]-referenceV[x])<.0002f);}
                }
            }
        }
        {
            // Independent cbrt reference, including saturated/dark colors.
            // The faster root must remain far below one output code level.
            float maximum = 0;
            alignas(16) float values[3][4];
            uint8_t pixels[16];
            for(int r=0;r<=255;r+=15)for(int g=0;g<=255;g+=15)for(int b=0;b<256;b+=4){
                for(int lane=0;lane<4;++lane){pixels[lane*4]=uint8_t(r);pixels[lane*4+1]=uint8_t(g);pixels[lane*4+2]=uint8_t(b+lane);pixels[lane*4+3]=255;}
                __m128 l,a,c;perceptualColor::forward4(pixels,l,a,c);
                _mm_store_ps(values[0],l);_mm_store_ps(values[1],a);_mm_store_ps(values[2],c);
                for(int lane=0;lane<4;++lane){const auto exact=perceptualColor::forward(pixels+lane*4);
                    for(int ch=0;ch<3;++ch)maximum=std::max(maximum,std::abs(values[ch][lane]-exact[ch]));}
            }
            std::cout<<"Fast color maximum error: "<<maximum<<'\n';
            check(maximum<.005f);
        }
        {
            FramePool pool;
            auto first=pool.acquire(17,23,17,23), held=first;
            std::fill(first->pixels.begin(),first->pixels.end(),197);
            const auto address=first->pixels.data();
            first.reset();
            auto next=pool.acquire(17,23,29,31);
            check(next->pixels.data()!=address&&held->pixels[0]==197);
            held.reset();
            auto reused=pool.acquire(17,23,37,41);
            check(reused->pixels.data()==address&&reused->pts==37&&reused->duration==41);
            auto resized=pool.acquire(19,29,0,1);resized->validate();
        }
        if(hasAvx2()){
            // Independent scalar lifting oracle covers fused inverse-column
            // boundaries, vector tails, narrow strips and untouched padding.
            std::mt19937 random(442);
            for(int width:{1,7,8,13,64})for(int height:{2,4,10,64}){
                const int stride=width+5,half=height/2;
                std::vector<float> image(size_t(stride)*height),scratch(image.size()),expected;
                for(auto& v:image)v=float(int(random()%20001)-10000)*.03125f;
                expected=image;
                for(int x=2;x<width+2;++x){
                    std::vector<float> lo(half),hi(half);
                    for(int y=0;y<half;++y){lo[y]=image[size_t(y)*stride+x]*(1/1.149604398f);hi[y]=image[size_t(y+half)*stride+x]*1.149604398f;}
                    auto lift=[&](bool low,float factor){auto& dst=low?lo:hi;const auto& src=low?hi:lo;
                        for(int y=0;y<half;++y)dst[y]+=factor*(src[y]+src[std::clamp(y+(low?-1:1),0,half-1)]);};
                    lift(true,-.4435068522f);lift(false,-.8829110762f);lift(true,.05298011854f);lift(false,1.586134342f);
                    for(int y=0;y<half;++y){expected[size_t(2*y)*stride+x]=lo[y];expected[size_t(2*y+1)*stride+x]=hi[y];}
                }
                waveletColumnRangeAvx2(image.data(),scratch.data(),stride,stride,height,2,width+2,true);
                check(image==expected);
            }
        }
        if(hasAvx2()){
            // Verify exact byte conversion at every exponent-bin boundary,
            // every quantization threshold and deterministic random bit patterns.
            std::vector<float> input{-1.f,0.f,1.f,2.f};
            const auto& packed=perceptualColor::gammaExponentByteTable();
            for(uint32_t i=0;i<packed.size();++i){
                const uint32_t start=(i+112*128)<<16,threshold=packed[i]>>8;
                for(auto bits:{start-1,start,start+65535,start+threshold-1,start+threshold,start+threshold+1})input.push_back(std::bit_cast<float>(bits));
            }
            std::mt19937 random(441);
            for(unsigned i=0;i<200000;++i)input.push_back(std::bit_cast<float>(uint32_t(random()%0x40000001u)));
            while(input.size()%8)input.push_back(0);
            std::vector<uint32_t> output(input.size());
            check(gammaByteRowAvx2(input.data(),uint32_t(input.size()),output.data())==input.size());
            const auto& table=perceptualColor::gammaTable();
            for(size_t i=0;i<input.size();++i){const float position=std::clamp(input[i],0.f,1.f)*8192;const auto at=uint32_t(position);const float f=position-at;
                check(output[i]==uint32_t(table[at]+f*(table[std::min(8192u,at+1)]-table[at])+.5f));}
        }
        auto codec = makeCodec(CodecId::Fajk);
        codec->setThreads(3);
        codec->setSharpening(0);
        {
            const auto before=workerLimit(1);
            ScopedWorkerBudget scope(2);
            check(workerLimit(0)==2&&workerLimit(100)==2&&workerLimit(1)==before);
            parallelFor(8,2,[&](uint32_t){check(workerLimit(0)==1);ScopedWorkerBudget nested(1);check(workerLimit(100)==1);});
            check(workerLimit(0)==2);
            std::atomic_uint32_t visits=0;
            parallelFor(32,2,[&](uint32_t){parallelFor(7,2,[&](uint32_t){++visits;});});
            check(visits==32*7);
        }
        for(uint32_t budget:{1u,2u,3u,6u}){
            ScopedWorkerBudget scope(budget);std::atomic_uint32_t active=0,peak=0,visits=0;
            const auto share=budget/std::min(3u,budget);
            parallelFor(3,budget,[&](uint32_t){
                check(workerLimit()==share);
                parallelFor(17,budget,[&](uint32_t){
                    const auto running=active.fetch_add(1)+1;auto old=peak.load();
                    while(old<running&&!peak.compare_exchange_weak(old,running)){}
                    for(int i=0;i<32;++i)std::this_thread::yield();
                    ++visits;--active;
                });
            });
            check(visits==51&&active==0&&peak<=budget&&workerLimit()==budget);
            // One outer task must retain parallelism, whereas an explicit
            // one-worker request must constrain all its children as well.
            parallelFor(1,budget,[&](uint32_t){check(workerLimit()==budget);});
            parallelFor(3,1,[&](uint32_t){check(workerLimit()==1);});
            rejected([&]{parallelFor(3,budget,[&](uint32_t){parallelFor(5,budget,[&](uint32_t i){if(!i)throw std::runtime_error("nested worker failure");});});});
            check(workerLimit()==budget);
        }
        {
            ScopedWorkerBudget scope(6);
            parallelFor(2,6,[&](uint32_t){check(workerLimit()==3);parallelFor(9,3,[&](uint32_t){check(workerLimit()==1);});});
        }
        // Automatic budgets must cover headers, padding and partial chroma.
        for(auto [w,h]:{std::pair{1u,1u},std::pair{3u,65u},std::pair{17u,23u},std::pair{64u,64u},std::pair{65u,67u}})
            for(float quality:{1.f,85.f,100.f}){
                Frame small{w,h,17,23,Bytes(pixelBytes(w,h),255)};
                for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=(size_t(y)*w+x)*4;
                    small.pixels[at]=uint8_t(x*4);small.pixels[at+1]=uint8_t(y*4);small.pixels[at+2]=uint8_t((x+y)*2);}
                EncodeOptions automatic;automatic.threads=1;automatic.quality=quality;automatic.automaticTarget=true;
                const auto coded=codec->encode(small,automatic);
                check(coded.data.size()<=qualityTargetBytes(w,h,quality));
                codec->decode(coded,w,h).validate();
            }
        for (auto size : {std::pair{1u, 1u}, std::pair{17u, 23u}, std::pair{65u, 67u}}) {
            for (bool alpha : {false, true}) {
                auto source = fixture(size.first, size.second, alpha);
                EncodeOptions o;
                o.lossless = true;
                o.threads = 3;
                const auto packet = codec->encode(source, o);
                check(codec->decode(packet, source.width, source.height).pixels == source.pixels);
                Bytes direct(source.pixels.size());
                decodeFrameToBuffer(packet, source.width, source.height, 1, 100, direct, 0, true);
                check(direct == source.pixels);
            }
        }
        Frame palette{33, 19, 17, 23, Bytes(pixelBytes(33, 19), 255)};
        for (size_t i = 0; i < palette.pixels.size(); i += 4)
            palette.pixels[i] = uint8_t(i % 3 * 80);
        EncodeOptions lossless;
        lossless.lossless = true;
        check(codec->decode(codec->encode(palette, lossless), 33, 19).pixels == palette.pixels);
        const auto source = fixture(513, 517);
        {
            Frame solid{513,517,17,23,Bytes(pixelBytes(513,517),37)};
            EncodeOptions compact;compact.targetBytes=6000;compact.budgetHeatmap=std::make_shared<BudgetHeatmap>();
            const auto coded=codec->encode(solid,compact);
            check(coded.data.size()<64&&coded.data[4]==4);
            check(codec->decode(coded,solid.width,solid.height).pixels==solid.pixels);
            check(compact.budgetHeatmap->bits.empty());
            compact.targetBytes=1;rejected([&]{codec->encode(solid,compact);});
        }
        for (uint32_t budget : {6000u, 12000u, 30000u}) {
            EncodeOptions o;
            o.targetBytes = budget;
            o.threads = 3;
            o.quality = 100;
            const auto packet = codec->encode(source, o);
            check(packet.data.size() <= budget && packet.data[4] == 130);
            o.budgetHeatmap = std::make_shared<BudgetHeatmap>();
            check(codec->encode(source, o).data == packet.data);
            const auto& map = *o.budgetHeatmap;
            // The map uses the encoded working grid; the UI stretches that
            // grid along with the image when low-rate input was downsampled.
            check(map.width > 0 && map.width <= source.width && map.height > 0 && map.height <= source.height &&
                  map.bits.size() == size_t(map.cols) * map.rows);
            double mappedBits = 0;
            for (auto bits : map.bits) { check(std::isfinite(bits) && bits >= 0); mappedBits += bits; }
            check(mappedBits > 0 && map.mappedBytes <= packet.data.size() &&
                  std::abs(mappedBits / 8 - map.mappedBytes) < .1);
            const auto image = codec->decode(packet, source.width, source.height);
            image.validate();
            check(image.pts == 17 && image.duration == 23);
            Bytes guarded(image.pixels.size() + 32, 197);
            decodeFrameToBuffer(packet, source.width, source.height, 1, 100,
                                std::span(guarded).subspan(16, image.pixels.size()), 0, true);
            check(std::equal(image.pixels.begin(), image.pixels.end(), guarded.begin() + 16));
            for (int i = 0; i < 16; ++i)
                check(guarded[i] == 197 && guarded[guarded.size() - 1 - i] == 197);
        }
        // Fast changes quantization decisions, not frame/alpha semantics or
        // strict size caps. Worker count must not change its bitstream.
        for(bool aq:{false,true})for(bool subsample:{false,true}){
            auto fastSource=fixture(129,131);
            EncodeOptions fast;fast.speed=EncodeSpeed::Fast;fast.quality=100;
            fast.targetBytes=5000;fast.threads=1;fast.chroma420=subsample;fast.adaptiveQuantizationEnabled=aq;
            const auto first=codec->encode(fastSource,fast);check(first.data.size()<=fast.targetBytes);
            fast.threads=3;check(codec->encode(fastSource,fast).data==first.data);
            auto image=codec->decode(first,fastSource.width,fastSource.height);
            image.validate();check(image.pts==fastSource.pts&&image.duration==fastSource.duration);
        }
        {
            auto fastAlpha=fixture(65,67,true);EncodeOptions fast;fast.speed=EncodeSpeed::Fast;
            fast.quality=100;fast.targetBytes=12000;fast.threads=2;
            auto packet=codec->encode(fastAlpha,fast);check(packet.data.size()<=fast.targetBytes);
            auto image=codec->decode(packet,65,67);
            for(size_t at=3;at<image.pixels.size();at+=4)check(image.pixels[at]==fastAlpha.pixels[at]);
            fast.lossless=true;fast.targetBytes=0;
            check(codec->decode(codec->encode(fastAlpha,fast),65,67).pixels==fastAlpha.pixels);
            fast.speed=EncodeSpeed(255);rejected([&]{codec->encode(fastAlpha,fast);});
        }
        auto alpha = fixture(65, 67, true);
        EncodeOptions o;
        o.quality = 100;
        o.targetBytes = 12000;
        const auto alphaImage = codec->decode(codec->encode(alpha, o), 65, 67);
        for (size_t i = 3; i < alpha.pixels.size(); i += 4)
            check(alphaImage.pixels[i] == alpha.pixels[i]);
        o.quality = std::numeric_limits<float>::quiet_NaN();
        rejected([&] { codec->encode(source, o); });
        std::mt19937 random(1);
        for (size_t count : {0u, 1u, 63u, 64u, 1024u, 4096u, 65536u}) {
            Bytes raw(count);
            for (auto& b : raw)
                b = uint8_t(random());
            for (bool compact : {false, true})
                check(entropyDecode(entropyEncode(raw, compact), count) == raw);
        }
        rejected([&] { pixelBytes(0, 1); });
        rejected([&] { pixelBytes(16385, 1); });
        check(frameTimestamp(30000, 30000, 1001) == 10010000000ll);
        const auto path = std::filesystem::temp_directory_path() / L"fajk-current-container-test.fajk";
        {
            SequenceInfo info{33, 19, 30, 1, CodecId::Fajk, 1, 0, 0};
            SequenceWriter writer(path, info, true);
            writer.append(codec->encode(palette, lossless));
            writer.finish();
        }
        {
            SequenceReader reader(path);
            check(reader.info().count == 1 && reader.info().width == 33 && reader.info().height == 19);
            check(codec->decode(reader.read(0), 33, 19).pixels == palette.pixels);
            rejected([&] { reader.read(1); });
        }
        std::filesystem::remove(path);
        std::cout << "PASS current R441: lossless RGBA/palette, alpha, strict budgets, guarded decode and thread "
                     "equality, entropy, timestamps, container and validation.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
