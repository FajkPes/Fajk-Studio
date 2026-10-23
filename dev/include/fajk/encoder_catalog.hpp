#pragma once
#include "types.hpp"
#include <array>
namespace fajk {
struct EncoderDescription {EncoderRevision revision;const wchar_t* name;const char* profile;const wchar_t* summary;const wchar_t* details;ComputeBackend backend=ComputeBackend::Cpu;};
inline constexpr auto defaultEncoderRevision=EncoderRevision::Experimental011441R441;
inline constexpr std::array encoderCatalog{
    EncoderDescription{defaultEncoderRevision,L"R500 · CPU","r490",L"R500 s výpočtem na procesoru.",L"R500 s AVX2 rekonstrukcí a rychlejším RDO. Komprese i obnova obrazu využívají procesor. Stejný formát jako R500 GPU, kompatibilní s R470."},
    EncoderDescription{defaultEncoderRevision,L"R500 GPU","r490-gpu",L"R500 s GPU analýzou a převzorkováním.",L"Společná volba GPU pro kompresi i dekódování. GPU počítá barvy, analýzu, vhodné převzorkování a první tři úrovně dopředné transformace nad rezidentními rovinami. CPU zpracuje zbylé malé úrovně, volbu přesnosti a entropické kódování. Dekodér využívá GPU pro wavelet, nativní filtry a obnovu barev. Finální ostření běží po rekonstrukci, s rychlou AVX2 nebo vhodnou GPU cestou. Malé obrazy a nepodporované operace využijí CPU. Samostatné snímky zachovávají formát R470; volitelná temporal videa vyžadují nový dekodér.",ComputeBackend::Gpu}
};
inline size_t encoderIndex(EncoderRevision,ComputeBackend backend=ComputeBackend::Cpu){return backend==ComputeBackend::Gpu?1:0;}
}
