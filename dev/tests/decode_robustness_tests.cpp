#include "fajk/codec.hpp"
#include "../src/decode_internal.hpp"
#include "../src/spatial.hpp"
#include <iostream>
#include <random>
#include <source_location>

using namespace fajk;
namespace {
void check(bool value, std::source_location loc = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Decode robustness assertion at " + std::to_string(loc.line()));
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
void decodeCase(PacketView packet, uint32_t width, uint32_t height) {
    const auto size = pixelBytes(width, height);
    Bytes guarded(size + 32, 197);
    Frame owned;
    bool acceptedOwned = true, acceptedDirect = true;
    try {
        owned = decodeFrameWithSettings(packet, width, height, 1, 100, 0, true);
    } catch (const std::exception&) {
        acceptedOwned = false;
    }
    try {
        decodeFrameToBuffer(packet, width, height, 3, 100, std::span(guarded).subspan(16, size), 0, true);
    } catch (const std::exception&) {
        acceptedDirect = false;
    }
    check(acceptedOwned == acceptedDirect);
    for (size_t i = 0; i < 16; ++i)
        check(guarded[i] == 197 && guarded[guarded.size() - 1 - i] == 197);
    if (acceptedOwned) {
        owned.validate();
        check(owned.pts == packet.pts && owned.duration == packet.duration);
        check(std::equal(owned.pixels.begin(), owned.pixels.end(), guarded.begin() + 16));
    }
}
} // namespace
int main() {
    try {
        // Switching magnitude codes must be lossless for signed integers,
        // sparse/dense bands, predictor modes, partial tiles and guarded output.
        bool selectedShort=false;
        std::mt19937 modelRandom(460);
        for(auto [w,h]:{std::pair{1,1},std::pair{7,9},std::pair{17,23},std::pair{64,65}})
            for(bool prediction:{false,true})for(int density:{1,4,16}){
                std::vector<float> integers(size_t(w)*h,0.f);
                for(auto& value:integers)if(modelRandom()%16<unsigned(density))value=float(int(modelRandom()%9)-4);
                std::vector<float> costs,selected;
                auto original=encodeSpatialBand(integers.data(),w,w,h,1.f,prediction,.5f,false,0,true,0,true,true,.02f,nullptr,0,nullptr,0,23,
                    0,0,0,nullptr,0,false,90,{},0,{},&costs,&selected);
                auto compact=compactSpatialBand(original,w,h,prediction,&costs);
                check(compact.size()<=original.size());if(compact.empty())continue;
                // The diagnostic map must follow the chosen entropy model,
                // including its real final byte count, without changing bytes.
                check(compact==compactSpatialBand(original,w,h,prediction));
                check(selected==integers&&compact==compactSpatialBand(original,w,h,prediction,nullptr,selected));
                double costSum=0;for(auto cost:costs){check(std::isfinite(cost)&&cost>=0);costSum+=cost;}
                check(std::abs(costSum-compact.size()*8.)<.02);
                selectedShort|=compact.front()==6;
                std::vector<float> guarded(integers.size()+16,197.f);
                std::fill(guarded.begin()+8,guarded.end()-8,0.f);
                decodeSpatialBand(compact,guarded.data()+8,w,w,h,1.f,false,prediction,false,false,true,true,nullptr,0,31);
                check(std::equal(integers.begin(),integers.end(),guarded.begin()+8));
                for(int i=0;i<8;++i)check(guarded[i]==197&&guarded[guarded.size()-1-i]==197);
                auto bad=compact;bad[0]=8;
                rejected([&]{decodeSpatialBand(bad,guarded.data()+8,w,w,h,1.f,false,prediction,false,false,true,true,nullptr,0,31);});
                if(compact.front()!=4)rejected([&]{decodeSpatialBand(compact,guarded.data()+8,w,w,h,1.f,false,prediction,false,true,true,true,nullptr,0,31);});
            }
        check(selectedShort);
        {
            // Medium magnitudes exercise the third exact representation.
            constexpr int w=64,h=65;
            std::vector<float> integers(w*h),selected,decoded(w*h);
            for(size_t i=0;i<integers.size();++i)integers[i]=float((i%2?-1:1)*(4+int(modelRandom()%4)));
            auto coded=encodeSpatialBand(integers.data(),w,w,h,1.f,false,.5f,false,0,true,0,true,true,.02f,nullptr,0,nullptr,0,23,
                0,0,0,nullptr,0,false,90,{},0,{},nullptr,&selected);
            const auto compact=compactSpatialBand(coded,w,h,false,nullptr,selected);
            check(compact.size()<=coded.size());
            decodeSpatialBand(compact,decoded.data(),w,w,h,1.f,false,false,false,false,true,true,nullptr,0,31);
            check(decoded==integers);
            check(compactSpatialBand(compact,w,h,false)==compact);
            auto wider=encodeSpatialBand(integers.data(),w,w,h,1.f,false,.5f,false,0,true,0,true,true,.02f,nullptr,0,nullptr,0,26);
            check(!wider.empty());wider[0]=7;std::fill(decoded.begin(),decoded.end(),0);
            decodeSpatialBand(wider,decoded.data(),w,w,h,1.f,false,false,false,false,true,true,nullptr,0,31);
            check(decoded==integers);
            rejected([&]{decodeSpatialBand(wider,decoded.data(),w,w,h,1.f,false,false,false,true,true,true,nullptr,0,31);});
        }

        {
            // Flat and protected groups share one entropy tile, but must not
            // share its RDO price. Include partial final groups/tiles.
            constexpr int w=17,h=23,cols=(w+3)/4;
            std::vector<float> source(w*h,.9f), rates(cols*((h+3)/4)), selected, pilot, decoded(w*h);
            for(size_t i=0;i<rates.size();++i)rates[i]=(i%cols)%2 ? .03125f : 128.f;
            auto encode=[&](bool select){return encodeSpatialBand(source.data(),w,w,h,1.f,false,.5f,false,0,true,0,true,true,
                .02f,nullptr,0,nullptr,0,23,.1f,0,0,nullptr,0,false,90,{},0,{},nullptr,
                select?&pilot:&selected,select,rates);};
            auto coded=encode(false);
            check(encode(true).empty()&&pilot==selected);
            for(int y=0;y<h;++y)for(int x=0;x<w;++x)
                check(selected[y*w+x]==((x/4)%2?1.f:0.f));
            decodeSpatialBand(coded,decoded.data(),w,w,h,1.f,false,false,false,false,true,true,nullptr,0,23);
            check(decoded==selected);
            rates.pop_back();rejected([&]{encode(false);});
        }
        // Capturing final integers must also include RDO decisions on real
        // nonintegral coefficients, not merely repeat initial nearest rounding.
        for(bool prediction:{false,true})for(float step:{1.f,3.5f}){
            constexpr int w=17,h=23;std::vector<float> source(w*h),selected,decoded(w*h,0.f);
            for(auto& value:source)value=(int(modelRandom()%2001)-1000)*.013f;
            auto original=encodeSpatialBand(source.data(),w,w,h,step,prediction,.5f,false,0,true,0,true,true,.02f,nullptr,0,nullptr,0,23,
                .1f,.125f,0,nullptr,0,false,90,{},0,{},nullptr,&selected);
            const auto compact=compactSpatialBand(original,w,h,prediction,nullptr,selected);
            check(compact==compactSpatialBand(original,w,h,prediction));
            if(!compact.empty())decodeSpatialBand(compact,decoded.data(),w,w,h,1.f,false,prediction,false,false,true,true,nullptr,0,31);
            check(selected==decoded);
            std::vector<float> pilot;
            const auto discarded=encodeSpatialBand(source.data(),w,w,h,step,prediction,.5f,false,0,true,0,true,true,.02f,
                nullptr,0,nullptr,0,23,.1f,.125f,0,nullptr,0,false,90,{},0,{},nullptr,&pilot,true);
            check(discarded.empty() && pilot==selected);
        }

        // Native local AQ captures reconstructed base-step units, including
        // fractional precisions, partial tiles and the model-only pilot path.
        for (int w : {7, 17}) for (float step : {1.f, 3.5f}) for (bool prediction : {false,true}) {
            constexpr int h = 23;
            const int cols = (w + 7) / 8;
            std::vector<float> source(w*h), selected, pilot, decoded(w*h), precisions, costs;
            std::vector<float> rates(cols * ((h+7)/8));
            for (auto& v : source) v = (int(modelRandom()%2001)-1000)*.013f;
            for (size_t i=0;i<rates.size();++i) rates[i] = i%2 ? .03125f : 128.f;
            auto encode = [&](bool selection, bool capture) {
                return encodeSpatialBand(source.data(),w,w,h,step,prediction,.5f,false,0,true,3,true,true,.02f,
                    nullptr,0,nullptr,0,32,.01f,.125f,0,nullptr,0,true,90,rates,1,{},
                    capture ? &costs : nullptr,selection ? &pilot : &selected,selection,{},
                    capture ? &precisions : nullptr);
            };
            const auto coded = encode(false,true);
            check(!coded.empty() && precisions.size()==rates.size() && costs.size()==rates.size());
            for(float p : precisions) check(p==0 || p==.25f || p==.5f || p==1 || p==2 || p==4);
            decodeSpatialBand(coded,decoded.data(),w,w,h,1.f,false,prediction,false,true,true,true,nullptr,0,32);
            check(decoded==selected);
            check(encode(false,false)==coded);
            check(encode(true,false).empty() && pilot==selected);
            rejected([&] {decodeSpatialBand(coded,decoded.data(),w,w,h,1.f,false,prediction,false,false,true,true,nullptr,0,32);});
        }
        {
            // Extra flat savings must change selected coefficients after the
            // classifier saturates, while source-protected groups stay exact.
            constexpr int w=16,h=16;
            for(uint32_t rateCell : {1u,2u,4u}) {
            const int cols=w/rateCell;
            std::vector<float> source(w*h,1.9f), rates(cols*cols), baseline, stronger;
            for(size_t i=0;i<rates.size();++i) rates[i]=i%cols<size_t(cols/2) ? 128.f : .03125f;
            auto encode=[&](float boost,std::vector<float>& selected) {
                return encodeSpatialBand(source.data(),w,w,h,1.f,false,.5f,false,0,true,0,true,true,
                    .02f,nullptr,0,nullptr,0,23,.001f,0,0,nullptr,0,false,90,{},0,{},nullptr,
                    &selected,false,rates,nullptr,boost,rateCell);
            };
            const auto a=encode(1,baseline), b=encode(64,stronger);
            double before=0, after=0;
            for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
                if(x<8) {before+=std::abs(baseline[y*w+x]);after+=std::abs(stronger[y*w+x]);}
                else check(baseline[y*w+x]==stronger[y*w+x]);
            }
            check(after<before && a!=b);
            }
        }
        // Shared validation rejects malformed geometry/modes before dereferencing buffers.
        for (uint8_t mode : {uint8_t(19), uint8_t(22), uint8_t(31), uint8_t(255)}) {
            SpatialCoderConfig config{mode, true, true, false, 0};
            rejected([&] { config.validate(17, 19, 17, 1); });
        }
        SpatialCoderConfig config;
        for (float step : {0.f, -1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
            rejected([&] { config.validate(17, 19, 17, step); });
        rejected([&] { config.validate(17, 19, 16, 1); });
        rejected([&] { SpatialCoderConfig{1, true, true, true, 8}.validate(17, 19, 17, 1); });

        Frame source{65, 67, 17, 23, Bytes(pixelBytes(65, 67), 255)};
        std::mt19937 random(441);
        for (uint32_t y = 0; y < source.height; ++y)
            for (uint32_t x = 0; x < source.width; ++x) {
                const auto at = (size_t(y) * source.width + x) * 4;
                for (int c = 0; c < 3; ++c)
                    source.pixels[at + c] = uint8_t((x * 7 + y * 3 + c * 47) % 256);
            }
        auto codec = makeCodec(CodecId::Fajk);
        size_t cases = 0;
        for (int variant = 0; variant < 16; ++variant) {
            const auto revision = EncoderRevision::Experimental011441R441;
            EncodeOptions options;
            options.revision = revision;
            options.quality = variant == 0 ? 10 : variant == 1 ? 50 : 100;
            options.lossless = variant == 3;
            options.threads = 1;
            options.targetBytes = 12000;
            options.automaticTarget = false;
            if(variant>=4){
                options.flatAllocation=std::array<uint32_t,4>{0,49,51,100}[(variant-4)%4];
                const auto chroma=(variant-4)/4;
                options.chroma420=chroma==0;options.chroma422=chroma==1;
            }
            const auto valid = codec->encode(source, options);
            decodeCase(valid, source.width, source.height);
            ++cases;
            // Read from an unaligned, bounded subspan rather than an owned vector.
            Bytes nested(7, 211);
            nested.insert(nested.end(), valid.data.begin(), valid.data.end());
            nested.push_back(211);
            decodeCase({std::span(nested).subspan(7, valid.data.size()), 17, 23}, source.width, source.height);
            ++cases;
            for (size_t n = 0; n < 32; ++n) {
                decodeCase({std::span(valid.data).first(std::min(n, valid.data.size())), 17, 23}, 65, 67);
                ++cases;
            }
            for (int mutation = 0; mutation < 192; ++mutation) {
                auto damaged = valid;
                switch (mutation % 4) {
                case 0:
                    damaged.data.resize(random() % damaged.data.size());
                    break;
                case 1:
                    damaged.data[random() % damaged.data.size()] ^= uint8_t(1u << (random() % 8));
                    break;
                case 2:
                    for (int i = 0; i < 4; ++i)
                        damaged.data[random() % damaged.data.size()] = uint8_t(random());
                    break;
                case 3:
                    damaged.data.insert(damaged.data.begin() + random() % damaged.data.size(), uint8_t(random()));
                    break;
                }
                decodeCase(damaged, source.width, source.height);
                ++cases;
            }
        }
        for (unsigned version = 0; version < 256; ++version) {
            Bytes header(16);
            header[4] = uint8_t(version);
            decodeCase({header, 17, 23}, 65, 67);
            ++cases;
        }
        std::cout << "PASS: " << cases
                  << " deterministic decode cases; guarded owned/direct and worker equality, borrowed subspans, "
                     "truncation, mutations, all dispatch bytes, spatial validation.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
