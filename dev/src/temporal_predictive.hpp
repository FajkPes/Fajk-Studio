#pragma once
#include "fajk/types.hpp"
#include "packet_view.hpp"
namespace fajk {
struct PredictiveTrial { Packet packet; Frame image,witness; Bytes ages; uint32_t retained=0,blocks=0,selectedStep=0,rateRetries=0; double motionMs=0,transformMs=0,codingMs=0,predictionMse=0; size_t decisionBytes=0; bool overBudget=false; };
bool predictiveOpaque(const Frame&);
enum class MotionReferenceFilter:uint8_t {None,Legacy,Adjacent};
// Production Max settings. Historical residual experiments keep the lower-
// level entry point below; production callers do not pass positional flags.
struct MotionResidualSettings {
    ComputeBackend backend=ComputeBackend::Cpu;
    uint32_t tolerance=6,step=1,maximumStep=0,holdFrames=10,errorCarry=0;
    size_t maximumBytes=SIZE_MAX;
    MotionReferenceFilter filter=MotionReferenceFilter::Adjacent;
    bool residentPreparation=true,illuminationPrediction=true;
};
PredictiveTrial trialMotion(const Frame&,const Frame& sourceKey,const Frame& decodedKey,
                           uint32_t workers,const MotionResidualSettings&,std::span<const uint8_t> ages={});
// Signed residual experiments share the implementation with Max compression.
// Container version 137 accepts variants 14/15/16; earlier variants remain
// diagnostic formats and are not accepted by the public frame dispatch.
// The byte ceiling applies before lossless motion-metadata normalization, so
// that optimization cannot change reference/key choices or decoded quality.
// Optional maximumStep permits bounded requantization of an over-budget affine
// residual. It reuses the immutable transform and all GPU preparation. Large
// sampled predictor errors disable coarse retries, preserving a key fallback.
PredictiveTrial trialPredictive(const Frame&,const Frame& sourceKey,const Frame& decodedKey,
                               uint32_t workers,uint32_t tolerance,uint32_t step,ComputeBackend,
                               std::span<const uint8_t> ages={},uint32_t hold=10,uint32_t propagatedError=0,bool compact=false,uint32_t curve=65,const Frame* codedTarget=nullptr,bool blockTransform=false,bool gpuBlocks=false,bool affine=false,size_t maximumBytes=SIZE_MAX,bool residentPreparation=true,bool illuminationPrediction=true,bool compactMotion=true,uint32_t maximumStep=0,bool inLoopFilter=false,bool adjacentFilter=false);
Frame decodePredictiveTrial(PacketView,const Frame& decodedKey,uint32_t workers);
}
