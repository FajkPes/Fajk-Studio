#pragma once
#include "fajk/types.hpp"
#include <array>
#include <cstdint>
#include <stdexcept>
namespace fajk {
struct WaveletProfile {
    int kernel = 10, ratePasses = -1, synthesisRatePasses = -2;
    WaveletStage synthesisStage = WaveletStage::Pilot;
    uint8_t lumaStrength = 0, strength = 8, slope = 136, chroma = 154, planes = 3, diagonal = 173, prediction = 255,
            rgb = 0, adaptive = 0, guide = 0, entropyMode = 0, colorSpace = 0, coupledQuantizer = 0, contrastLoss = 0,
            colorFeedbackPlanes = 3, spatialParts = 1, sourcePriority = 0, approximatePilot = 0;
    bool fastColor = false, rateQuantizer = false, median = false, enhanced = false, extended = false, parent = false,
         reuseSynthesisSource = false, fitRateCap = false, noReconstructionBias = false;
    float chromaDetailThreshold = 0, cflPilotGain = 0, rdo = 0, adaptiveRate = .02f,
          rounding = .5f, quantRdo = 0, quantGradient = 0, synthesisGain = 0, quantCoupling = 0, colorFeedback = 0,
          pilotStepScale = 1, middleBandGain = 0;
};
inline WaveletProfile resolveWaveletProfile(WaveletStage stage, bool lowRate) {
    require(stage == WaveletStage::Final || stage == WaveletStage::Pilot, "Invalid wavelet stage.");
    const bool final = stage == WaveletStage::Final;
    WaveletProfile p;
    p.kernel = 10;
    p.ratePasses = final ? 3 : -1;
    p.synthesisRatePasses = final ? 1 : -2;
    p.lumaStrength = final ? 8 : 0;
    // Low-rate reconstruction already cleans RGB. At high density keep a
    // restrained native dering pass: removing it entirely exposes ringing.
    // Existing packets retain their own stored controls.
    p.strength = final && !lowRate ? 24 : 0;
    p.slope = 166;
    p.chroma = lowRate ? 160 : 144;
    p.planes = 3;
    p.diagonal = 173;
    p.prediction = 4;
    p.rgb = 0;
    p.adaptive = 3;
    p.guide = 8;
    p.entropyMode = final ? 23 : 0;
    p.colorSpace = 2;
    p.coupledQuantizer = lowRate ? 0 : 3;
    p.contrastLoss = 0;
    p.colorFeedbackPlanes = 3;
    p.spatialParts = 1;
    p.sourcePriority = 22;
    p.approximatePilot = 0;
    p.fastColor = 1;
    p.rateQuantizer = lowRate ? 1 : 0;
    p.median = 0;
    p.enhanced = 1;
    p.extended = 1;
    p.parent = 0;
    p.reuseSynthesisSource = final ? (lowRate ? 1 : 0) : (lowRate ? 0 : 0);
    p.fitRateCap = final ? 1 : 0;
    p.noReconstructionBias = false;
    p.chromaDetailThreshold = 0.f;
    p.cflPilotGain = 0.f;
    p.rdo = 0.f;
    p.adaptiveRate = 0.0199999996f;
    p.rounding = 0.5f;
    p.quantRdo = 0.0399999991f;
    p.quantGradient = 0.125f;
    p.synthesisGain = final ? 0.375f : 0.f;
    p.quantCoupling = 1.f;
    p.colorFeedback = 0.f;
    p.pilotStepScale = 1.f;
    // Counter the low-rate loss of medium-scale structure before quantization.
    // The finest band is deliberately excluded: amplifying it strengthens noise.
    p.middleBandGain = final && lowRate ? .125f : 0.f;
    return p;
}
} // namespace fajk
