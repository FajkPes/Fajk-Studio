#pragma once
#include "types.hpp"
#include <array>
namespace fajk {
struct EncoderDescription {EncoderRevision revision;const wchar_t* name;const char* profile;const wchar_t* summary;const wchar_t* details;ComputeBackend backend=ComputeBackend::Cpu;};
inline constexpr auto defaultEncoderRevision=EncoderRevision::Experimental011441R441;
inline constexpr std::array encoderCatalog{
    EncoderDescription{defaultEncoderRevision,L"R600.4 · CPU","r590",L"R600.4 s čištěním barevného šumu na procesoru.",L"Komprese i obnova obrazu využívají procesor a AVX2. Jemnější ukládání barev, prostorové odšumění a kontrola pohybových artefaktů v režimu Max. Fast / More quality zůstávají samostatnou volbou."},
    EncoderDescription{defaultEncoderRevision,L"R600.4 GPU","r590-gpu",L"R600.4 s GPU akcelerací a čištěním barevného šumu.",L"Společná volba GPU pro kompresi, náhled a přehrávání. GPU počítá barvy, analýzu, vhodné převzorkování a velké dopředné transformace. Nativní dekodér drží inverzi, prostorové čištění barev a obnovu RGB na GPU. Max využívá GPU pro hledání pohybu, afinní predikci a spojenou přípravu rezidua s transformací. Pohybový dekodér spojuje predikci, přesnou inverzi, barvy a referenční filtr na GPU. CPU zpracuje entropii, malé úrovně transformace a kontrolu kvality predikce. Nepodporované operace automaticky využijí CPU. Jemnější barevný kvantizér a kontrola lokálních barevných bloků omezují šum; velmi nízký datový limit může dál ztrácet detaily a časovou stabilitu.",ComputeBackend::Gpu}
};
inline size_t encoderIndex(EncoderRevision,ComputeBackend backend=ComputeBackend::Cpu){return backend==ComputeBackend::Gpu?1:0;}
}
