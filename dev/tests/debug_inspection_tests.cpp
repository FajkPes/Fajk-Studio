#include "../src/debug_inspection.hpp"
#include "../src/wavelet_debug.hpp"
#include "../src/wavelet.hpp"
#include "../src/decode_internal.hpp"
#include "../src/adaptive_allocation.hpp"
#include "../src/source_priority.hpp"
#include "fajk/codec.hpp"
#include <iostream>
using namespace fajk;
template <class F> void rejects(F&& f) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, "Inspector accepted invalid input.");
}
int main() {
    try {
        BudgetHeatmap estimateCurrent, estimateReference;
        estimateCurrent.width = 4; estimateCurrent.height = 2; estimateCurrent.cell = 2;
        estimateCurrent.cols = 2; estimateCurrent.rows = 1;
        estimateCurrent.bits = {20, 80}; estimateCurrent.flatCoverage = {1, 0};
        estimateReference.width = 8; estimateReference.height = 4; estimateReference.cell = 2;
        estimateReference.cols = 4; estimateReference.rows = 2;
        estimateReference.bits = {10,10,10,10,20,20,20,20};
        auto savings = compareFlatAllocation(estimateCurrent, estimateReference);
        require(savings.referenceBits == 60 && savings.currentBits == 20 && savings.areaShare == .5 &&
                    savings.referenceOtherBits == 60 && savings.currentOtherBits == 80,
                "Flat savings compared different regions across working resolutions.");
        estimateCurrent.width = 3; estimateReference.width = 5;
        estimateReference.cols = 3; estimateReference.bits = {10,10,10,20,20,20};
        estimateCurrent.flatCoverage = {1,1};
        savings = compareFlatAllocation(estimateCurrent, estimateReference);
        require(std::abs(savings.referenceBits - 90) < .0001 && savings.currentBits == 100 && savings.areaShare == 1,
                "Flat savings lost partial border cells or hid negative savings.");
        estimateCurrent.flatCoverage = {0,0};
        require(compareFlatAllocation(estimateCurrent, estimateReference).referenceBits == 0,
                "Flat savings counted non-flat pixels.");
        estimateReference.bits[0] = -1;
        rejects([&] { compareFlatAllocation(estimateCurrent, estimateReference); });
        require(std::abs(flatMinimumCoverage(0)-.95f)<.0001f && std::abs(flatMinimumCoverage(100)-.25f)<.0001f,
                "Threshold does not widen mostly-flat eligibility.");
        rejects([&] {flatMinimumCoverage(101);});
        EncodeOptions allocationOptions;
        allocationOptions.adaptiveQuantizationEnabled=true;
        for (int i = 0; i <= 100; ++i) {
            const float c = i / 100.f;
            require(allocationFlatCoverage(c, 50) == c, "50% altered baseline classification.");
            require(allocationFlatCoverage(c, 0) <= c && allocationFlatCoverage(c, 100) >= c,
                    "Flat tolerance moves in the wrong direction.");
        }
        require(allocationFlatCoverage(.25f, 100) == 1 && allocationFlatCoverage(.75f, 0) == 0 &&
                    allocationFlatCoverage(0, 100) == 0,
                "Mixed areas are not reclassified as requested.");
        const float edgeBaseline = adaptiveAllocationRate(.25f, .5f, false, allocationOptions);
        allocationOptions.flatAllocation = 100;
        const float fullySmoothRate = adaptiveAllocationRate(1, 1, false, allocationOptions);
        const float quarterSmoothRate = adaptiveAllocationRate(1, .25f, false, allocationOptions);
        require(quarterSmoothRate > 1 && quarterSmoothRate < std::sqrt(fullySmoothRate),
                "Mixed eligibility applied full-flat savings to mostly non-flat detail.");
        require(adaptiveAllocationRate(.25f, .5f, false, allocationOptions) > edgeBaseline &&
                    adaptiveAllocationRate(.25f, 0, false, allocationOptions) == .25f,
                "Savings skipped an edge-halo mixed area or damaged a fully protected footprint.");
        allocationOptions.flatAllocation = 0;
        require(adaptiveAllocationRate(1, 1, false, allocationOptions) < 1,
                "Conservative setting failed to preserve more precision in smooth areas.");
        Frame wall{64, 64, 0, 0, Bytes(pixelBytes(64, 64), 255)};
        uint32_t random = 12345;
        for (size_t i = 0; i < wall.pixels.size(); i += 4) {
            random = random * 1664525u + 1013904223u;
            const int noise = int(random >> 28) - 8;
            wall.pixels[i] = uint8_t(150 + noise);
            wall.pixels[i + 1] = uint8_t(135 + noise);
            wall.pixels[i + 2] = uint8_t(120 + noise);
        }
        SourcePriority wallPriority(wall, 1, 31);
        require(wallPriority.flatCoverage(16, 16, 16, 16, 100) > wallPriority.flatCoverage(16, 16, 16, 16, 50),
                "RGB low-contrast wall cannot become eligible at higher tolerance.");
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x) {
                auto* p = wall.pixels.data() + (size_t(y) * 64 + x) * 4;
                p[0] = x < 36 ? 230 : 20;
                p[1] = x < 36 ? 15 : 180;
                p[2] = x < 36 ? 25 : 230;
            }
        SourcePriority colorBoundary(wall, 1, 31);
        require(colorBoundary.flatCoverage(32, 16, 8, 8, 100) == 0,
                "Tolerance classified a strong colored boundary as flat.");
        for (uint32_t y=0;y<64;++y) for (uint32_t x=0;x<64;++x) {
            auto* p=wall.pixels.data()+(size_t(y)*64+x)*4;
            for (int c=0;c<3;++c)p[c]=uint8_t((x<36?40:60)+5*c);
        }
        SourcePriority weakBoundary(wall,1,31);
        require(weakBoundary.hasDirectEdge(32,16,8,8) && !weakBoundary.hasDirectEdge(16,16,8,8) &&
                    weakBoundary.flatCoverage(32,16,8,8,100)==0,
                "Recognized low-contrast geometry lost its direct-edge protection.");
        for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x) {
            auto* p=wall.pixels.data()+(size_t(y)*64+x)*4;
            for(int c=0;c<3;++c)p[c]=uint8_t((x<36?12:20)+c*2);
        }
        SourcePriority darkBoundary(wall,1,31);
        require(darkBoundary.hasDirectEdge(32,16,8,8) && darkBoundary.flatCoverage(32,16,8,8,100)==0,
                "A coherent dark step was mistaken for flat shading.");
        for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x) {
            auto* p=wall.pixels.data()+(size_t(y)*64+x)*4;
            for(int c=0;c<3;++c)p[c]=uint8_t(12+x+c*2);
        }
        SourcePriority darkRamp(wall,1,31);
        require(!darkRamp.hasDirectEdge(32,16,8,8) && darkRamp.flatCoverage(32,16,8,8,100)==1,
                "A smooth dark ramp was incorrectly protected as a step.");
        Frame source{129, 97, 0, 1, Bytes(pixelBytes(129, 97), 255)};
        for (uint32_t y = 0; y < source.height; ++y)
            for (uint32_t x = 0; x < source.width; ++x) {
                auto* p = source.pixels.data() + (size_t(y) * source.width + x) * 4;
                p[0] = uint8_t(35 + x / 2);
                p[1] = uint8_t(40 + y);
                p[2] = uint8_t(70 + (x + y) % 90);
            }
        EncodeOptions options;
        options.adaptiveQuantizationEnabled=true;
        options.threads = 1;
        options.targetBytes = 6000;
        const auto raw = encodeWavelet(source, options, false);
        const auto rawBytes = raw.data;
        WaveletDebugCapture capture;
        capture.views = {3, 4};
        const auto inspected = inspectWavelet(raw, source.width, source.height, 1, capture);
        const auto ordinary = decodeWavelet(raw, source.width, source.height, 1);
        require(inspected.pixels == ordinary.pixels, "Capture altered native pixels.");
        for (const auto& image : capture.images) {
            image.validate();
            for (size_t i = 3; i < image.pixels.size(); i += 4)
                require(image.pixels[i] == 255, "Fast color path failed to capture components.");
        }
        capture.views = {2, 5};
        inspectWavelet(raw, source.width, source.height, 3, capture);
        for (const auto& image : capture.images)
            image.validate();
        auto codec = makeCodec(CodecId::Fajk);
        codec->setThreads(1);
        auto packet = codec->encode(source, options);
        const auto bytes = packet.data;
        const auto before = codec->decode(packet, source.width, source.height);
        for (int view = 0; view < int(debugViewNames.size()); view += 2) {
            const auto debug = inspectPacket(packet, source.width, source.height,
                                             {view, std::min(view + 1, int(debugViewNames.size()) - 1)}, 2, 1);
            debug.comparison.validate();
            require(debug.report.find("Accounted: " + std::to_string(packet.data.size()) + " B") != std::string::npos,
                    "Packet sections do not sum to the packet size.");
            if (view == 0)
                for (uint32_t y = 0; y < source.height; ++y)
                    for (uint32_t x = 0; x < source.width; ++x)
                        for (int c = 0; c < 4; ++c)
                            require(debug.comparison.pixels[(size_t(y) * debug.comparison.width + x) * 4 + c] ==
                                        before.pixels[(size_t(y) * source.width + x) * 4 + c],
                                    "Final debug view changed pixels.");
        }
        require(packet.data == bytes && raw.data == rawBytes &&
                    codec->decode(packet, source.width, source.height).pixels == before.pixels,
                "Inspector mutated packet or decoder state.");
        require(codec->encode(source, options).data == bytes, "Inspector changed subsequent encoding.");
        const auto a = inspectPacket(packet, source.width, source.height, {2, 4}, 1, 1),
                   b = inspectPacket(packet, source.width, source.height, {2, 4}, 1, 3);
        require(a.comparison.pixels == b.comparison.pixels, "Worker-dependent diagnostic output.");
        DebugInspectionCache cache;
        const auto uncached=inspectPacket(packet,source.width,source.height,{0,15},1,1,nullptr,&cache);
        const auto reused=inspectPacket(packet,source.width,source.height,{0,15},1,3,nullptr,&cache);
        require(uncached.comparison.pixels==reused.comparison.pixels&&cache.packet==packet.data,
                "Cached inspection changed pixels.");
        auto damaged=packet;damaged.data.resize(5);
        rejects([&]{inspectPacket(damaged,source.width,source.height,{0,15},1,1,nullptr,&cache);});
        require(inspectPacket(packet,source.width,source.height,{0,15},1,1,nullptr,&cache).comparison.pixels==uncached.comparison.pixels,
                "Failed inspection poisoned cache.");
        // Assert actual encoded chroma and decoded component dimensions, not
        // merely a UI setting. Odd dimensions exercise independent padding.
        const auto chroma420 = inspectPacket(packet, source.width, source.height, {5, 6});
        require(chroma420.report.find("Chroma: 4:2:0") != std::string::npos &&
                    chroma420.captions[0].find("65 x 49") != std::string::npos,
                "Default chroma layout changed.");
        options.chroma420 = false;
        const auto fullColor = codec->encode(source, options);
        const auto chroma444 = inspectPacket(fullColor, source.width, source.height, {5, 6});
        require(chroma444.report.find("Chroma: 4:4:4") != std::string::npos &&
                    chroma444.captions[0].find("129 x 97") != std::string::npos,
                "Full chroma request was silently subsampled.");
        options.chroma420 = true;
        require(codec->encode(source, options).data == bytes, "Returning to 4:2:0 changed encoded bytes.");
        options.chroma420 = false;
        options.chroma422 = true;
        const auto horizontalColor = codec->encode(source, options);
        const auto chroma422 = inspectPacket(horizontalColor, source.width, source.height, {5, 6});
        require(chroma422.report.find("Chroma: 4:2:2") != std::string::npos &&
                    chroma422.captions[0].find("65 x 97") != std::string::npos,
                "4:2:2 does not preserve vertical chroma resolution.");
        options.chroma422 = false;
        options.chroma420 = true;
        options.budgetHeatmap = std::make_shared<BudgetHeatmap>();
        require(codec->encode(source, options).data == bytes, "AQ diagnostics changed packet bytes.");
        const auto map = options.budgetHeatmap;
        double displayedBits=0;
        for(float bits : map->bits) displayedBits+=bits;
        require(std::abs(displayedBits-8*map->mappedBytes)<.1 &&
                    std::abs(map->mappedBytes+map->unmappedDetailBytes-map->detailBytes)<.01,
                "Spatial attribution lost or duplicated entropy bytes.");
        double tileBits=0;
        for (const auto& tile:map->tiles) {
            require(tile.width && tile.height && tile.x+tile.width<=map->width && tile.y+tile.height<=map->height &&
                        tile.channel<3 && tile.level<6 && std::isfinite(tile.bits) && tile.bits>=0,
                    "Invalid captured entropy tile.");
            tileBits+=tile.bits;
        }
        require(!map->tiles.empty() && tileBits<=8*map->detailBytes+.02 &&
                    std::abs(map->mappedBytes+map->unmappedDetailBytes-map->detailBytes)<.01,
                "Band tile report lost or duplicated coded bits.");
        require(!map->bits.empty() && map->penalties[0].size() == map->bits.size() &&
                    map->localQuantSteps[0].size() == map->bits.size() &&
                    map->flatCoverage.size() == map->bits.size(),
                "Missing actual AQ capture.");
        const auto debugBudget = inspectPacket(packet, source.width, source.height, {16, 17}, 1, 1, map.get());
        debugBudget.comparison.validate();
        const auto initialWeights = map->penalties;
        // Exercise the native quantizer directly: the full small-image router
        // may downsample this synthetic 8px pattern before its source analysis.
        // A constrained encode must change real bytes, not only diagnostic colors.
        Frame allocationSource{257, 193, 0, 0, Bytes(pixelBytes(257, 193), 255)};
        uint32_t seed = 9876;
        for (uint32_t y = 0; y < allocationSource.height; ++y)
            for (uint32_t x = 0; x < allocationSource.width; ++x) {
                auto* p = allocationSource.pixels.data() + (size_t(y) * allocationSource.width + x) * 4;
                seed = seed * 1664525u + 1013904223u;
                const int mode = (x / 60 + y / 60) % 3;
                if (mode == 0) {
                    p[0] = uint8_t(35 + x / 2);
                    p[1] = uint8_t(45 + y / 2);
                    p[2] = 95;
                } else if (mode == 1) {
                    const int n = int(seed >> 28) - 8;
                    p[0] = uint8_t(150 + n);
                    p[1] = uint8_t(135 + n);
                    p[2] = uint8_t(120 + n);
                } else {
                    p[0] = x % 8 < 4 ? 230 : 20;
                    p[1] = x % 8 < 4 ? 15 : 180;
                    p[2] = x % 8 < 4 ? 25 : 230;
                }
            }
        options.targetBytes = 1200;
        encodeWavelet(allocationSource, options, false);
        const auto baselineEligibility = map->flatCoverage;
        const auto baselineWeights = map->penalties;
        options.flatAllocation = 100;
        const auto aggressivePacket = encodeWavelet(allocationSource, options, false);
        for (float price : map->allocationRates)
            require(price == .03125f || price == 128.f,
                    "Maximum contrast did not reach the actual two-class RDO prices.");
        const auto aggressiveEligibility = map->flatCoverage;
        const auto aggressiveWeights = map->penalties;
        options.flatAllocation = 0;
        const auto conservativePacket = encodeWavelet(allocationSource, options, false);
        decodeWavelet(conservativePacket, allocationSource.width, allocationSource.height, 1).validate();
        decodeWavelet(aggressivePacket, allocationSource.width, allocationSource.height, 1).validate();
        bool savesFlat = false, protectsEdge = false;
        for (size_t i = 0; i < baselineEligibility.size(); ++i) {
            require(map->flatCoverage[i] <= baselineEligibility[i] &&
                        aggressiveEligibility[i] >= baselineEligibility[i],
                    "Real encoder eligibility map is not monotonic.");
            for (int c = 0; c < 3; ++c) {
                savesFlat |= aggressiveWeights[c][i] > baselineWeights[c][i];
                protectsEdge |= aggressiveWeights[c][i] < baselineWeights[c][i];
            }
            for (int c = 0; c < 3; ++c) {
                require(map->penalties[c][i] >= .03125f && map->penalties[c][i] <= 128.f &&
                            aggressiveWeights[c][i] >= .03125f && aggressiveWeights[c][i] <= 128.f,
                        "Real encoder penalties escaped the supported range.");
                // A displayed 16-pixel cell now averages several independent
                // RDO groups. Mixed cells need not move monotonically: edge
                // weights fall while flat weights rise inside that same cell.
            }
        }
        require(savesFlat && protectsEdge, "Allocation failed to separate smooth and actual edge precision.");
        require(aggressiveEligibility != baselineEligibility, "Aggressive allocation did not change eligibility.");
        require(map->flatCoverage != baselineEligibility, "Conservative allocation did not change eligibility.");
        require(aggressivePacket.data != conservativePacket.data,
                "Allocation failed to change constrained encoded data.");
        for (const auto& channel : map->localQuantSteps) {
            require(channel.size()==map->bits.size(),"Missing actual local precision map.");
            for (float step : channel)
                require(std::isfinite(step) && (step==0 || (step>=.25f && step<=4.f)),"Invalid actual local precision.");
        }
        std::string qlegend;
        const auto qview=budgetDebugImage(*map,17,qlegend);
        qview.validate();
        options.flatAllocation = 50;
        options.targetBytes = 6000;
        require(codec->encode(source, options).data == bytes, "50% failed to restore baseline encoded bytes.");
        options.adaptiveAllocationStrength = 0;
        codec->encode(source, options);
        for (const auto& plane : map->penalties)
            for (float weight : plane)
                require(weight == 1, "Allocation control did not reach diagnostic weights.");
        require(initialWeights != map->penalties, "AQ diagnostic did not respond to allocation strength.");
        options.adaptiveAllocationStrength = 100;
        Frame smooth = source;
        for (size_t i = 0; i < smooth.pixels.size(); i += 4) {
            smooth.pixels[i] = 45;
            smooth.pixels[i + 1] = 65;
            smooth.pixels[i + 2] = 95;
            smooth.pixels[i + 3] = 255;
        }
        options.flatSavings = 0;
        encodeWavelet(smooth, options, false);
        const auto smoothWeights = map->penalties;
        options.flatSavings = 200;
        encodeWavelet(smooth, options, false);
        require(smoothWeights != map->penalties, "Smooth-area savings did not reach AQ weights.");
        for (uint32_t y = 0; y < map->rows; ++y)
            for (uint32_t x = 0; x < map->cols; ++x) {
                const auto area =
                    std::min(map->cell, map->width - x * map->cell) * std::min(map->cell, map->height - y * map->cell);
                // The production classifier deliberately leaves <16-pixel border
                // fragments unknown; do not mistake that conservative guard for a
                // failure to recognize an adequately sampled constant region.
                if (area >= 16)
                    require(map->flatCoverage[size_t(y) * map->cols + x] > .99f,
                            "Smooth source classification is incorrect.");
            }
        // Native RDO is active with and without a target, in all chroma modes.
        for (int mode = 0; mode < 3; ++mode)
            for (float quality : {15.f, 85.f}) {
                auto varied = options;
                varied.quality = quality;
                varied.targetBytes = 0;
                varied.flatSavings = 0;
                varied.chroma420 = mode == 0;
                varied.chroma422 = mode == 1;
                varied.flatAllocation = 50;
                encodeWavelet(allocationSource, varied, false);
                const auto originalPenalties = map->penalties;
                varied.flatAllocation = 100;
                const auto variedPacket = encodeWavelet(allocationSource, varied, false);
                decodeWavelet(variedPacket, allocationSource.width, allocationSource.height, 1).validate();
                require(originalPenalties != map->penalties, "Allocation disappeared at a quality or chroma mode.");
            }
        for (int chroma = 0; chroma < 3; ++chroma) for (float quality : {15.f, 85.f}) {
            auto disabled = options;
            disabled.adaptiveQuantizationEnabled = false;
            disabled.targetBytes = 0;
            disabled.quality = quality;
            disabled.chroma420 = chroma == 0;
            disabled.chroma422 = chroma == 1;
            disabled.flatAllocation = 0;
            const auto uniform = encodeWavelet(allocationSource, disabled, false);
            disabled.flatAllocation = 100;
            disabled.adaptiveAllocationStrength = 500;
            disabled.adaptiveQuantization = 1000;
            disabled.flatSavings = 500;
            disabled.flatThreshold = 100;
            const auto ignored = encodeWavelet(allocationSource, disabled, false);
            require(uniform.data == ignored.data, "AQ off still applied allocation controls.");
            for (const auto& plane : map->penalties) for (float weight : plane)
                require(weight == 1, "AQ off retained nonuniform source weights.");
            decodeWavelet(ignored, allocationSource.width, allocationSource.height, 1).validate();
        }
        options.flatSavings = 0;
        options.lossless = true;
        codec->encode(smooth, options);
        require(map->bits.empty(), "Palette encode leaked an earlier wavelet diagnostic map.");
        options.budgetHeatmap.reset();
        options.lossless = true;
        source.pixels[3] = 37;
        const auto lossless = codec->encode(source, options);
        const auto alpha = inspectPacket(lossless, source.width, source.height, {14, 15});
        require(alpha.comparison.pixels[0] == 37, "Alpha view lost source alpha.");
        rejects([&] { inspectPacket(packet, source.width, source.height, {-1, 2}); });
        rejects([&] { inspectPacket(packet, source.width, source.height, {1, 2}, 0); });
        auto broken = packet;
        broken.data.pop_back();
        rejects([&] { inspectPacket(broken, source.width, source.height, {1, 2}); });
        std::cout << "Debug capture, packet accounting, alpha, tails, workers and no mutation PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
