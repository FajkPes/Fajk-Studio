#pragma once
#include "fajk/types.hpp"
#include <optional>
#include "coefficient_overrides.hpp"

namespace fajk {
inline constexpr uint8_t coefficientTemporalVersion=136;
inline constexpr uint32_t coefficientMaxDepth=32; // Wire-format compatibility bound.
inline constexpr uint32_t coefficientSeekDepth=16; // Encoder bound, including static-repeat links.
inline constexpr int64_t temporalMaxKeyDuration=50'000'000; // Five seconds in 100 ns units.
inline bool isTemporalPacketVersion(uint8_t version){return version==132||version==133||version==coefficientTemporalVersion||version==137;}
// Lossless recoding of an already quantized independent image. The decoded
// coefficients, reconstruction controls and final image remain unchanged.
// Helpers can recode against independent packets; streaming can additionally
// use version-3 chains bounded to 32 dependencies and five seconds of changes.
struct CoefficientTemporalResult { Packet packet; uint32_t predictedBands=0; };
struct CoefficientEncodingInfo {uint32_t width=0,height=0;float step=0;};
std::optional<CoefficientEncodingInfo> coefficientEncodingInfo(const Packet&);
std::optional<CoefficientTemporalResult> encodeCoefficientTemporal(const Packet& current,const Packet& key,uint32_t workers);
Packet expandCoefficientTemporal(std::span<const uint8_t> data,const Packet& key,uint32_t workers);
Packet expandCoefficientTemporal(std::span<const uint8_t> data,std::shared_ptr<const Packet> key,uint32_t workers);
struct CoefficientTemporalExpansion {Packet packet;CoefficientBandOverrides coefficients;std::shared_ptr<const void> storage;};
CoefficientTemporalExpansion prepareCoefficientTemporal(std::span<const uint8_t> data,std::shared_ptr<const Packet> key,uint32_t workers,std::shared_ptr<const Packet> identity={});
class CoefficientTemporalEncoder {
    struct State;
    std::unique_ptr<State> state_;
    std::unique_ptr<State> previewState_;
    Packet previewPacket_;
public:
    // Optional root reuse restarts a bounded chain from its existing key;
    // it cannot extend the five-second/interval refresh of changed pictures.
    explicit CoefficientTemporalEncoder(uint32_t workers,uint32_t interval,bool chains=false,bool reuseRootAtDepth=false,bool exhaustiveReferences=false);
    ~CoefficientTemporalEncoder();
    Packet encode(Packet current,uint64_t index);
    // Evaluate an exact alternative without making it a future reference.
    // Only commit when its packet is actually written to the sequence.
    const Packet& preview(const Packet& current,uint64_t index);
    Packet commitPreview();
    void discardPreview();
    uint32_t encodedDepth() const;
};
}
