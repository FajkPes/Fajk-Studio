#include "debug_inspection.hpp"
#include "wavelet_debug.hpp"
#include "wavelet.hpp"
#include "decode_internal.hpp"
#include "reconstruction441.hpp"
#include "flat_regions.hpp"
#include "artifact_cleanup.hpp"
#include "artifact_reconstruction.hpp"
#include "fajk/entropy.hpp"
#include "fajk/hardware.hpp"
#include "fajk/pixels.hpp"
#include <sstream>
#include <cstring>
#include <iomanip>

namespace fajk {
FlatAllocationSavings compareFlatAllocation(const BudgetHeatmap& current, const BudgetHeatmap& reference) {
    auto valid = [](const BudgetHeatmap& m) {
        return m.width && m.height && m.cell && m.cols == (m.width + m.cell - 1) / m.cell &&
               m.rows == (m.height + m.cell - 1) / m.cell && m.bits.size() == size_t(m.cols) * m.rows;
    };
    require(valid(current) && valid(reference) && current.flatCoverage.size() == current.bits.size(),
            "Invalid allocation comparison maps.");
    const size_t stride = size_t(reference.cols) + 1;
    std::vector<double> prefix(stride * (reference.rows + 1));
    for (uint32_t y = 0; y < reference.rows; ++y)
        for (uint32_t x = 0; x < reference.cols; ++x) {
            const float bits = reference.bits[size_t(y) * reference.cols + x];
            require(std::isfinite(bits) && bits >= 0, "Invalid allocation bit cost.");
            const size_t i = (size_t(y) + 1) * stride + x + 1;
            prefix[i] = bits + prefix[i - 1] + prefix[i - stride] - prefix[i - stride - 1];
        }
    // The final cell can be narrower than cell. Preserve its full bit cost
    // when the normalized source rectangle includes the entire border cell.
    auto gridCoordinate = [](double p, uint32_t extent, uint32_t cell) {
        const uint32_t full = extent / cell, tail = extent % cell;
        return tail && p >= double(full) * cell ? full + (p - double(full) * cell) / tail : p / cell;
    };
    auto integral = [&](double x, double y) {
        x = std::clamp(gridCoordinate(x, reference.width, reference.cell), 0., double(reference.cols));
        y = std::clamp(gridCoordinate(y, reference.height, reference.cell), 0., double(reference.rows));
        const uint32_t ix = uint32_t(x), iy = uint32_t(y),
                       nx = std::min(reference.cols, ix + 1), ny = std::min(reference.rows, iy + 1);
        const double fx = x - ix, fy = y - iy;
        return (1 - fy) * ((1 - fx) * prefix[size_t(iy) * stride + ix] + fx * prefix[size_t(iy) * stride + nx]) +
               fy * ((1 - fx) * prefix[size_t(ny) * stride + ix] + fx * prefix[size_t(ny) * stride + nx]);
    };
    FlatAllocationSavings result;
    const double sx = double(reference.width) / current.width, sy = double(reference.height) / current.height;
    for (uint32_t y = 0; y < current.rows; ++y)
        for (uint32_t x = 0; x < current.cols; ++x) {
            const size_t i = size_t(y) * current.cols + x;
            require(std::isfinite(current.bits[i]) && current.bits[i] >= 0 &&
                    std::isfinite(current.flatCoverage[i]) && current.flatCoverage[i] >= 0 && current.flatCoverage[i] <= 1,
                    "Invalid allocation comparison cell.");
            if (current.flatCoverage[i] < .95f)
                continue;
            const double x0 = x * current.cell, y0 = y * current.cell,
                         x1 = std::min(current.width, (x + 1) * current.cell),
                         y1 = std::min(current.height, (y + 1) * current.cell);
            result.currentBits += current.bits[i];
            result.referenceBits += std::max(0., integral(x1*sx, y1*sy) - integral(x0*sx, y1*sy) -
                                                integral(x1*sx, y0*sy) + integral(x0*sx, y0*sy));
            result.areaShare += (x1 - x0) * (y1 - y0);
        }
    result.areaShare /= double(current.width) * current.height;
    double referenceTotal = 0, currentTotal = 0;
    for (float bits : reference.bits) referenceTotal += bits;
    for (float bits : current.bits) currentTotal += bits;
    result.referenceOtherBits = std::max(0., referenceTotal - result.referenceBits);
    result.currentOtherBits = std::max(0., currentTotal - result.currentBits);
    return result;
}
std::string flatAllocationSavingsText(const FlatAllocationSavings& s, bool extraSavings) {
    const double saved = s.referenceBits - s.currentBits;
    std::ostringstream out;
    out << std::fixed << std::setprecision(0)
        << (extraSavings ? "Plochy proti dodatečné úspoře 0 %: " : "Plochy proti kontrastu 0 %: ") << s.referenceBits << " → " << s.currentBits << " bitů.\n"
        << (saved >= 0 ? "Uvolněno přibližně " : "Navíc přibližně ") << std::abs(saved) << " bitů ("
        << std::setprecision(2) << std::abs(saved) / 8000. << " kB); bílé plochy " << s.areaShare * 100 << " %.\n"
        << std::setprecision(0) << "Zbytek obrazu: " << s.referenceOtherBits << " → " << s.currentOtherBits
        << " bitů (změna " << std::showpos << std::setprecision(2)
        << (s.currentOtherBits - s.referenceOtherBits) / 8000. << std::noshowpos << " kB).\n"
        << "Stejný rozpočet a ostatní volby. Stejná bílá maska (≥95 %); odhad z prostorově přiřazených detailů, "
           "bez DC a hlaviček. Nejde o zmenšení celého souboru ani důkaz, že všechny bity dostaly hrany.";
    return out.str();
}
Frame budgetDebugImage(const BudgetHeatmap& map, int view, std::string& legend) {
    const size_t cells = size_t(map.cols) * map.rows;
    require(map.width && map.height && map.cell && map.cols == (map.width + map.cell - 1) / map.cell &&
                map.rows == (map.height + map.cell - 1) / map.cell && map.bits.size() == cells,
            "Invalid budget debug map.");
    require(view >= 16 && view <= 21, "Invalid budget debug view.");
    if (view == 21) {
        require(map.allocationCols && map.allocationRows &&
                    map.allocationRates.size() == size_t(map.allocationCols) * map.allocationRows,
                "Missing actual allocation decisions.");
        Frame image{map.allocationCols,map.allocationRows,0,0,Bytes(pixelBytes(map.allocationCols,map.allocationRows))};
        for(size_t i=0;i<map.allocationRates.size();++i) {
            const float t=std::clamp((std::log2(std::max(.03125f,map.allocationRates[i]))+5.f)/12.f,0.f,1.f);
            auto* p=image.pixels.data()+i*4;
            p[0]=uint8_t(255*(1-t)); p[1]=uint8_t(210*t); p[2]=0; p[3]=255;
        }
        legend="ALOKACE: červená = chráněný detail, levná cena bitu (0,03125); zelená = vysoká cena bitu (128). "
               "Skutečné ceny nejjemnějších RDO skupin před dodatečnou úsporou. Při nízkém toku a kontrastu 100 % dvě krajní skupiny. "
               "Jde o rozhodnutí enkodéru, nikoli mapu zakódovaných bitů; ta je v pohledu Spotřeba dat.";
        return image;
    }
    Frame image{map.cols, map.rows, 0, 0, Bytes(pixelBytes(map.cols, map.rows))};
    const double limit = std::max(.125, map.mappedBytes * 8 / (double(map.width) * map.height) * 4);
    std::ostringstream text;
    text << std::fixed << std::setprecision(3);
    if (view == 16)
        text << "DATA: zelená = 0 bpp, oranžová = " << limit / 2 << " bpp, červená ≥ " << limit
             << " bpp. Pracovní buňka 16×16: 0 / " << limit / 2 * map.cell * map.cell << " / "
             << limit * map.cell * map.cell << " bitů. Y detail ~" << map.channelBits[0] << " bitů; C1 ~"
             << map.channelBits[1] << " bitů; C2 ~" << map.channelBits[2] << " bitů. Hlavičky/DC jsou zvlášť. "
             << "Cena bloků přiřazená podle ponechaných koeficientů; rozdělení uvnitř bloku je odhad. "
             << "Mimo viditelný obraz (padding) ~" << map.unmappedDetailBytes * 8 << " bitů.";
    else if (view == 20)
        text << "PLOCHY: černá = nezařazená oblast, bílá = plocha způsobilá k úspoře. Šedá = částečné zařazení. Mapa "
                "zahrnuje toleranci posuvníku; nejde o počet bitů. Skutečné náklady jsou v pohledu Spotřeba dat.";
    else
        text << (view == 17   ? "JAS Y. "
                 : view == 18 ? "BARVA C1: první barevná osa. "
                              : "BARVA C2: druhá barevná osa. ")
             << "Skutečný místní krok: zelená = jemný (0,25×), šedá = základní (1×), červená = hrubý (4×). "
                "Černá = žádné nenulové detaily v nejjemnějším pásmu. Průměr aktivních směrů v buňce; "
                "základní krok se mezi pásmy a snímky liší. Cena dat je v pohledu Spotřeba dat.";
    for (size_t i = 0; i < cells; ++i) {
        auto* p = image.pixels.data() + i * 4;
        p[3] = 255;
        if (view == 20) {
            require(map.flatCoverage.size() == cells, "Missing smooth classifier map.");
            p[0] = p[1] = p[2] = uint8_t(std::clamp(map.flatCoverage[i], 0.f, 1.f) * 255 + .5f);
        } else if (view >= 17) {
            const auto& values = map.localQuantSteps[view - 17];
            require(values.size() == cells, "Missing AQ map.");
            if (values[i] == 0) {
                p[0] = p[1] = p[2] = 0;
                continue;
            }
            const float signedValue = std::clamp(std::log2(std::max(.03125f, values[i])) * .5f, -1.f, 1.f);
            p[0] = uint8_t(96 + 159 * std::max(0.f, signedValue));
            p[1] = uint8_t(96 + 159 * std::max(0.f, -signedValue));
            p[2] = uint8_t(96 * (1 - std::abs(signedValue)));
        } else {
            const uint32_t x = uint32_t(i % map.cols) * map.cell, y = uint32_t(i / map.cols) * map.cell;
            const double area = std::min(map.cell, map.width - x) * std::min(map.cell, map.height - y);
            const float t = float(std::clamp(map.bits[i] / area / limit, 0., 1.));
            if (t < .5f) {
                p[0] = uint8_t(t * 510);
                p[1] = uint8_t(210 - t * 80);
                p[2] = uint8_t(80 * (1 - t * 2));
            } else {
                p[0] = uint8_t(255 - (t - .5f) * 50);
                p[1] = uint8_t(170 - (t - .5f) * 300);
                p[2] = uint8_t((t - .5f) * 80);
            }
        }
    }
    legend = text.str();
    return image;
}
std::string budgetCellInfo(const BudgetHeatmap& map, uint32_t x, uint32_t y) {
    require(map.width && map.height && map.cell && map.cols && map.rows &&
                map.bits.size() == size_t(map.cols) * map.rows,
            "Invalid cell probe.");
    x = std::min(x, map.width - 1);
    y = std::min(y, map.height - 1);
    const uint32_t cx = x / map.cell, cy = y / map.cell;
    const size_t at = size_t(cy) * map.cols + cx;
    const double area = std::min(map.cell, map.width - cx * map.cell) * std::min(map.cell, map.height - cy * map.cell);
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << "Buňka " << cx << ", " << cy << ": ~" << map.bits[at] << " bitů / "
         << map.bits[at] / area << " bpp";
    if (map.penalties[0].size() == map.bits.size() && map.penalties[1].size() == map.bits.size() &&
        map.penalties[2].size() == map.bits.size())
        text << " | váha Y=" << map.penalties[0][at] << " C1=" << map.penalties[1][at]
             << " C2=" << map.penalties[2][at];
    if (map.flatCoverage.size() == map.bits.size())
        text << " | zařazení mezi plochy " << map.flatCoverage[at] * 100 << "%";
    if (map.localQuantSteps[0].size() == map.bits.size() &&
        map.localQuantSteps[1].size() == map.bits.size() && map.localQuantSteps[2].size() == map.bits.size())
        text << " | krok Y=" << map.localQuantSteps[0][at] << " C1=" << map.localQuantSteps[1][at]
             << " C2=" << map.localQuantSteps[2][at] << "× (0 = bez jemných detailů)";
    return text.str();
}
DebugInspection inspectPacket(PacketView packet, uint32_t width, uint32_t height, std::array<int, 2> views, float gain,
                              uint32_t workers, const BudgetHeatmap* budgetMap, DebugInspectionCache* cache) {
    pixelBytes(width, height);
    for (auto view : views)
        require(view >= 0 && view < int(debugViewNames.size()), "Invalid debug view.");
    require(std::isfinite(gain) && gain >= 1 && gain <= 16, "Invalid debug gain.");
    workers = safeWorkers(width, height, false, workers);
    // Validate the complete packet with the production decoder. The independent
    // inspector never weakens its parser or replaces the displayed normal image.
    const bool cached = cache && cache->final.width == width && cache->final.height == height &&
                        cache->packet.size() == packet.data.size() &&
                        std::equal(cache->packet.begin(), cache->packet.end(), packet.data.begin());
    Frame ownedFinal;
    if(!cached){
        ownedFinal = decodeFrameWithSettings(packet, width, height, workers, 100, 0);
        if(cache){
            cache->packet.assign(packet.data.begin(),packet.data.end());
            cache->final=std::move(ownedFinal);
        }
    }
    const auto& final=cache?cache->final:ownedFinal;
    DebugInspection result;
    std::ostringstream report;
    report << "Packet: " << packet.data.size() << " B | " << width << " x " << height << "\n"
           << "Offsets are zero-based bytes inside this frame packet, not container offsets.\n"
           << "Header + payload sections below are disjoint; nested wrappers are not counted twice.\n";
    size_t accounted = 0;
    std::vector<std::pair<size_t,size_t>> sections;
    std::array<uint64_t, 3> channelBytes{};
    uint64_t modelBytes = 0, flatBytes = 0, alphaBytes = 0;
    auto section = [&](const char* name, std::span<const uint8_t> data) {
        const size_t offset = size_t(data.data() - packet.data.data());
        require(offset <= packet.data.size() && data.size() <= packet.data.size() - offset, "Invalid debug section.");
        report << offset << " + " << data.size() << " B : " << name << '\n';
        accounted += data.size();
        if (!data.empty()) sections.emplace_back(offset,data.size());
        const std::string_view label = name;
        if (label.starts_with("Y "))
            channelBytes[0] += data.size();
        if (label.starts_with("C1 "))
            channelBytes[1] += data.size();
        if (label.starts_with("C2 "))
            channelBytes[2] += data.size();
        if (label == "Color model header" || label == "Compressed color slopes")
            modelBytes += data.size();
        if (label == "Flat region header" || label == "Flat region compressed map" || label == "Native flat regions")
            flatBytes += data.size();
        if (label == "Alpha")
            alphaBytes += data.size();
    };
    auto data = packet.data;
    uint32_t iw = width, ih = height, tile = 64, budget = uint32_t(packet.data.size()), contours = 200, surfaces = 0;
    ContourTuning shapes;
    Bytes map, flat;
    uint32_t flatWidth = 0, flatHeight = 0;
    bool native = false;
    for (int depth = 0; depth < 8; ++depth) {
        require(data.size() >= 5, "Truncated debug packet.");
        ByteReader in{data};
        in.bytes(4);
        const auto version = in.u8();
        if (version == 130) {
            const auto v = read441(data, iw, ih);
            budget = v.budget;
            shapes=v.settings.shapes;
            section("R441 reconstruction header", data.first(data.size() - v.child.size()));
            data = v.child;
        } else if (version == 129) {
            in.bytes(1);
            surfaces = in.u16();
            in.bytes(8);
            budget = in.u32();
            contours = in.u16();
            in.u16();
            auto child = in.bytes(in.u32());
            in.end();
            section("Base contour header", data.first(data.size() - child.size()));
            data = child;
        } else if (version == 28) {
            in.bytes(11);
            const auto w = in.u32(), h = in.u32();
            auto child = in.bytes(in.u32());
            auto trailer = data.subspan(in.pos);
            budget = in.u32();
            surfaces = in.u16();
            contours = in.u16();
            in.end();
            section("Working resolution header", data.first(size_t(child.data() - data.data())));
            section("Working reconstruction controls", trailer);
            iw = w;
            ih = h;
            data = child;
        } else if (version == 77) {
            const auto mode = in.u8();
            in.u16();
            in.bytes(8);
            const auto mapSize = in.u32(), childSize = in.u32();
            const auto header = data.first(in.pos);
            auto encoded = in.bytes(mapSize), child = in.bytes(childSize);
            in.end();
            section("Flat region header", header);
            section("Flat region compressed map", encoded);
            flat = entropyDecode(encoded, 34 + size_t((iw + 7) / 8) * ((ih + 7) / 8));
            flatWidth = iw;
            flatHeight = ih;
            if (mode == 2)
                restoreFlatRegionPrediction(flat, iw, ih);
            data = child;
        } else if (version == 55) {
            tile = in.u8();
            const auto mode = in.u8();
            auto encoded = in.bytes(in.var());
            section("Color model header", data.first(size_t(encoded.data() - data.data())));
            section("Compressed color slopes", encoded);
            const uint32_t cols = (iw + tile - 1) / tile;
            const size_t cells = size_t(cols) * ((ih + tile - 1) / tile) * 2;
            map = entropyDecode(encoded, cells);
            require(map.size() == cells, "Invalid debug color model.");
            if (mode & 4)
                for (size_t cell = 0; cell < cells / 2; ++cell)
                    for (int c = 0; c < 2; ++c) {
                        const auto at = cell * 2 + c;
                        const int a = cell % cols ? map[at - 2] : 0, b = cell >= cols ? map[at - cols * 2] : 0,
                                  d = cell >= cols && cell % cols ? map[at - cols * 2 - 2] : 0;
                        map[at] = uint8_t(map[at] + std::clamp(a + b - d, std::min(a, b), std::max(a, b)));
                    }
            data = in.bytes(in.data.size() - in.pos);
        } else if (version == 114) {
            native = true;
            report << "Chroma: "
                   << ((data[7] & 2)    ? "4:2:0"
                       : (data[7] & 64) ? "4:2:2"
                                        : "4:4:4")
                   << " (working resolution).\n";
            in.bytes(27);
            size_t headerEnd = in.pos;
            static constexpr const char* names[17]{
                "Native flat regions", "Y DC",  "Y layout",  "Y horizontal",  "Y vertical",
                "Y diagonal",          "C1 DC", "C1 layout", "C1 horizontal", "C1 vertical",
                "C1 diagonal",         "C2 DC", "C2 layout", "C2 horizontal", "C2 vertical",
                "C2 diagonal",         "Alpha"};
            section("Wavelet header", data.first(headerEnd));
            for (int stream = 0; stream < 17; ++stream) {
                const size_t start = in.pos;
                const auto size = in.var();
                section("Stream length", data.subspan(start, in.pos - start));
                const auto encoded = in.bytes(size);
                section(names[stream], encoded);
                if (stream == 0 && flat.empty() && !encoded.empty()) {
                    flat = entropyDecode(encoded, 34 + size_t((iw + 7) / 8) * ((ih + 7) / 8));
                    flatWidth = iw;
                    flatHeight = ih;
                }
            }
            in.end();
            break;
        } else {
            section("Lossless packet", data);
            break;
        }
    }
    require(accounted == packet.data.size(), "Incomplete debug section accounting.");
    std::sort(sections.begin(),sections.end());
    size_t cursor=0;
    for(auto [offset,size] : sections) {
        require(offset==cursor,"Overlapping or missing debug packet bytes.");
        cursor+=size;
    }
    require(cursor==packet.data.size(),"Unaccounted packet trailer.");
    if (native && map.empty())
        report << "No separate color predictor: stored chroma is the complete decoded color plane.\n";
    report << "Accounted: " << accounted << " B. Working image: " << iw << " x " << ih << ".\n"
           << "Y: decoded perceptual luma +128 (not a new RGB luminance conversion).\n"
           << "C1/C2 and residual: signed values, zero=128, gain=" << gain << ". Clipping is display-only.\n"
           << "Coefficients: padded multilevel layout, signed 20*asinh(value/4), zero=128. Not spatial pixels.\n"
           << "Slopes: raw signed model values, zero=128, 4 display levels per code.\n"
           << "Contour effect: base geometry in green, skirt in red; not the final recursive filter mask.\n"
           << "Encoded bytes: row-major byte grid; neither pixels nor a spatial budget map.\n";
    const auto packetCost = "Celý snímkový paket: " + std::to_string(packet.data.size()) + " B. Kontejner je zvlášť.";
    for (auto& cost : result.costs)
        cost = "Dopočítaná diagnostika: 0 B navíc v uloženém souboru.";
    result.costs[0] = result.costs[1] = "Dopočítaný obraz, 0 B samostatné kopie. " + packetCost;
    result.costs[15] = packetCost + " Toto je pohled na tytéž bajty, ne další kopie.";
    if (native)
        for (int c = 0; c < 3; ++c) {
            const auto cost =
                "Zakódované proudy této složky: " + std::to_string(channelBytes[c]) + " B (" +
                std::to_string(channelBytes[c] * 8) +
                " bitů). Sdílená hlavička a délky proudů jsou zvlášť. Různé pohledy na stejnou složku se nesčítají.";
            result.costs[c == 0 ? 2 : c == 1 ? 3 : 4] = cost;
            result.costs[7 + c] = cost;
            if (c)
                result.costs[4 + c] = cost + " Barevná predikce má případně společný model navíc.";
        }
    const auto modelCost =
        "Společný barevný model C1+C2 včetně jeho hlavičky: " + std::to_string(modelBytes) +
        " B. Samostatnou cenu každé osy nelze v jejich společném komprimovaném proudu přesně oddělit.";
    result.costs[10] = result.costs[11] = modelCost;
    for (int c = 0; c < 3; ++c)
        result.costs[17 + c] += " Volby místní přesnosti jsou součástí koeficientových proudů dané složky (celkem " +
                                std::to_string(channelBytes[c]) +
                                " B); jejich samostatnou cenu nelze oddělit od společného entropy kódování.";

    result.costs[13] =
        "Uložené mapy přesných ploch: " + std::to_string(flatBytes) + " B. Společné délkové položky jsou zvlášť.";
    result.costs[14] =
        native ? "Samostatný alfa proud: " + std::to_string(alphaBytes) + " B."
               : "Alfa je součástí společného bezeztrátového proudu; samostatnou cenu nelze přesně oddělit.";
    WaveletDebugCapture capture;
    capture.views = views;
    capture.gain = gain;
    Frame low;
    if (native) {
        bool need = false;
        for (auto view : views)
            need |= ((view >= 1 && view <= 9) || view == 12);
        if (need)
            low = inspectWavelet({data, packet.pts, packet.duration}, iw, ih, workers, capture, map, tile);
    }
    std::array<Frame, 2> images;
    for (size_t slot = 0; slot < 2; ++slot) {
        const int view = views[slot];
        auto& image = images[slot];
        result.captions[slot] = std::string{};
        if (view >= 16 && budgetMap) {
            std::string legend;
            image = budgetDebugImage(*budgetMap, view, legend);
            report << legend << '\n';
            result.legend += legend + "\n";
        } else if (view == 0)
            image = final;
        else if (view == 14) {
            image = final;
            for (size_t at = 0; at < image.pixels.size(); at += 4) {
                const auto a = image.pixels[at + 3];
                image.pixels[at] = image.pixels[at + 1] = image.pixels[at + 2] = a;
                image.pixels[at + 3] = 255;
            }
        } else if (view == 15) {
            const size_t step = (packet.data.size() + maxPixels - 1) / maxPixels,
                         count = (packet.data.size() + step - 1) / step;
            const uint32_t byteWidth = count <= 256 * 16384 ? 256 : 4096;
            image = {byteWidth, uint32_t((count + byteWidth - 1) / byteWidth), 0, 0, {}};
            image.pixels.assign(pixelBytes(image.width, image.height), 255);
            for (size_t i = 0; i < count; ++i)
                image.pixels[i * 4] = image.pixels[i * 4 + 1] = image.pixels[i * 4 + 2] = packet.data[i * step];
            if (step > 1)
                report << "Byte grid samples every " << step << " bytes.\n";
        } else if (native && view >= 2 && view <= 9)
            image = std::move(capture.images[slot]);
        else if (native && view == 1)
            image = low;
        else if (native && view == 12) {
            result.legend += "Kontury: zelená = síla geometrické rekonstrukce, červená = čištění barevného lemu Skirt. "
                             "Černá = bez zásahu.\n";
            const auto rate = artifactReconstruction::rateStrength(budget, width, height);
            const double eq = double(budget) * double(2560 * 1440) / (double(width) * height);
            const auto amount = artifactReconstruction::contourStrength(rate, eq, contours);
            image = contourEffectMap(low, contourGuideSpacing(rate * surfaces / 100.f, amount), amount, true,shapes);
        } else if ((view == 10 || view == 11) && !map.empty()) {
            image = {uint32_t((iw + tile - 1) / tile), uint32_t((ih + tile - 1) / tile), 0, 0, {}};
            image.pixels.resize(pixelBytes(image.width, image.height));
            for (size_t cell = 0; cell < map.size() / 2; ++cell) {
                const auto value =
                    uint8_t(std::clamp(std::lround(128 + int8_t(map[cell * 2 + view - 10]) * 4 * gain), 0l, 255l));
                auto* p = image.pixels.data() + cell * 4;
                p[0] = p[1] = p[2] = value;
                p[3] = 255;
            }
        } else if (view == 13 && !flat.empty()) {
            const auto layout = readFlatRegions(flat, flatWidth, flatHeight);
            image = {flatWidth, flatHeight, 0, 0, Bytes(pixelBytes(flatWidth, flatHeight), 0)};
            for (uint32_t y = 0; y < flatHeight; ++y)
                for (uint32_t x = 0; x < flatWidth; ++x) {
                    auto* p = image.pixels.data() + (size_t(y) * flatWidth + x) * 4;
                    const auto index = layout.map[size_t(y / layout.tile) * layout.cols + x / layout.tile];
                    if (index)
                        std::memcpy(p, &layout.palette[index], 4);
                    p[3] = 255;
                }
        } else {
            image = {iw, ih, 0, 0, Bytes(pixelBytes(iw, ih), 0)};
            for (size_t at = 3; at < image.pixels.size(); at += 4)
                image.pixels[at] = 255;
            result.captions[slot] += "V tomto paketu není k dispozici · ";
        }
        image.validate();
        result.captions[slot] += std::to_string(image.width) + " x " + std::to_string(image.height);
    }
    const uint32_t paneWidth = std::min(width, (maxDimension - 16) / 2), outWidth = paneWidth * 2 + 16,
                   outHeight = std::min(height, uint32_t(maxPixels / outWidth));
    auto& out = result.comparison;
    out = {outWidth, outHeight, packet.pts, packet.duration, Bytes(pixelBytes(outWidth, outHeight), 0)};
    for (size_t at = 3; at < out.pixels.size(); at += 4)
        out.pixels[at] = 255;
    for (size_t slot = 0; slot < 2; ++slot) {
        const auto& image = images[slot];
        for (uint32_t y = 0; y < outHeight; ++y)
            for (uint32_t x = 0; x < paneWidth; ++x) {
                const auto sx = uint32_t(uint64_t(x) * image.width / paneWidth),
                           sy = uint32_t(uint64_t(y) * image.height / outHeight);
                std::memcpy(out.pixels.data() + (size_t(y) * outWidth + slot * (paneWidth + 16) + x) * 4,
                            image.pixels.data() + (size_t(sy) * image.width + sx) * 4, 4);
            }
    }
    report << result.captions[0] << '\n' << result.captions[1] << '\n';
    result.report = report.str();
    return result;
}
} // namespace fajk
