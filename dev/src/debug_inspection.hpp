#pragma once
#include "packet_view.hpp"
#include <array>

namespace fajk {
inline constexpr std::array<const wchar_t*, 22> debugViewNames{L"Výsledný obraz",
                                                               L"Nativní RGB před konturami",
                                                               L"Jas Y",
                                                               L"Barva C1 po predikci",
                                                               L"Barva C2 po predikci",
                                                               L"Reziduum C1",
                                                               L"Reziduum C2",
                                                               L"Wavelet koeficienty Y",
                                                               L"Wavelet koeficienty C1",
                                                               L"Wavelet koeficienty C2",
                                                               L"Sklon barevného modelu C1",
                                                               L"Sklon barevného modelu C2",
                                                               L"Účinek kontur / Skirt",
                                                               L"Mapa přesných ploch",
                                                               L"Průhlednost alfa",
                                                               L"Zakódované bajty",
                                                               L"Skutečná spotřeba dat",
                                                               L"AQ · skutečný krok jasu",
                                                               L"AQ · skutečný krok C1",
                                                               L"AQ · skutečný krok C2",
                                                               L"AQ · plochy pro úsporu",
                                                               L"AQ · rozhodnutí o rozpočtu"};
inline constexpr std::array<const wchar_t*, 22> debugViewDescriptions{
    L"Hotový obraz po dekódování a rekonstrukci. Slouží ke kontrole výsledné kvality. RGB obrázek není uložen jako "
    L"další kopie.",
    L"Obraz v pracovním rozlišení před základní rekonstrukcí kontur. Pomáhá odlišit chyby komprese od účinku "
    L"vyhlazení.",
    L"Jasová složka Y před převodem do RGB. Nese většinu tvarů a jemných detailů. Zobrazuje skutečný výsledek inverzní "
    L"transformace.",
    L"První barevná osa C1 po přičtení barevné predikce. Spolu s C2 a jasem určuje výsledné barvy; nejde o samostatný "
    L"RGB kanál.",
    L"Druhá barevná osa C2 po barevné predikci. Pomáhá hledat barevné lemy a chyby rekonstrukce barev.",
    L"Dekódovaný rozdíl C1, který zbývá po barevné predikci z jasu. Bez samostatného modelu je to celá uložená barevná "
    L"složka. Šedá 128 znamená nulu.",
    L"Dekódovaný rozdíl C2 po barevné predikci. Ukazuje, kolik barevné informace musí přenést reziduum. Šedá 128 "
    L"znamená nulu.",
    L"Kvantované waveletové koeficienty jasu po dekódování. Rozložení obsahuje různá měřítka a směry, není to "
    L"prostorová mapa pixelů.",
    L"Waveletové koeficienty barevné složky C1. Nejde o další uloženou kopii vedle rezidua: jde o jeho zakódovanou "
    L"podobu.",
    L"Waveletové koeficienty barevné složky C2. Ukazují zakódovanou podobu stejné složky, kterou vidíte jako reziduum.",
    L"Místní sklon predikce C1 z jasu. Model se při dekódování interpoluje se sousedy. Obě barevné osy mají společně "
    L"komprimovaná pomocná data.",
    L"Místní sklon predikce C2 z jasu. V 4:2:2 / 4:4:4 současný model nemusí být přítomen. Cena společného modelu se "
    L"nesmí započítat dvakrát.",
    L"Zelená ukazuje váhu základní geometrie kontur, červená Skirt. Váhy se počítají z dekódovaného obrazu; není to "
    L"maska celého výsledného rekurzivního filtru.",
    L"Mapa přesně uložených konstantních oblastí. Jde o součást komprimovaného souboru; není to stejná mapa jako "
    L"tolerantní klasifikace ploch pro AQ.",
    L"Průhlednost obrazu. Bez uloženého alfa proudu je obraz implicitně neprůhledný a další alfa data nestojí nic.",
    L"Bajty komprimovaného snímkového paketu v pořadí v souboru. Není to prostorová mapa obrazu ani další uložená "
    L"vrstva.",
    L"Přibližně prostorově přiřazené náklady detailových proudů v bitech. Hlavičky a DC jsou zvlášť. Mapa vyžaduje "
    L"diagnostický encode a není uložená v souboru.",
    L"Skutečně zvolené místní kroky nejjemnějšího pásma jasu, průměr aktivních směrů. Zelená = 0,25×, šedá = 1×, "
    L"červená = 4× základního kroku; černá = žádné nenulové jemné detaily. Nejde o počet bitů ani další uloženou mapu.",
    L"Skutečné místní kroky první barevné složky. Při 4:2:0 / 4:2:2 pracuje barva ve větších oblastech než jas. "
    L"Menší krok dovoluje přesnější barevné hrany; černá znamená vynechané nejjemnější detaily.",
    L"Skutečné místní kroky druhé barevné složky. Obě barvy sdílejí zdrojovou strategii, ale volí přesnost samostatně "
    L"podle skutečných koeficientů a ceny kódování. Faktor není počet bitů.",
    L"Zařazení oblastí mezi plochy vhodné pro úsporu po započtení tolerance. Bílá znamená způsobilou plochu, šedá "
    L"částečné zařazení, černá nezařazenou oblast, nikoli nutně hranu. Detektor posuzuje všechny RGB složky a odchylku "
    L"od plynulého stínování.",
    L"Skutečné ceny nejjemnějších jasových RDO skupin. Červená chrání detail, zelená silně penalizuje data. Při nízkém toku a kontrastu 100 % "
    L"používá tento stupeň dvě krajní ceny. Rozhodnutí neznamená stejný počet bitů v každé skupině; data jsou v pohledu Spotřeba dat."};
struct DebugInspection {
    Frame comparison;
    std::string report, legend;
    std::array<std::string, 2> captions;
    std::array<std::string, 22> costs; // Shared representations overlap; never sum view prices.
};
// Explicit diagnostic-only cache, owned by one serial inspector worker.
// Full bytes and dimensions form the key; new packets always get validated.
struct DebugInspectionCache {
    Bytes packet;
    Frame final;
};
// Explicit, synchronous developer operation, never called by encode/decode.
DebugInspection inspectPacket(PacketView, uint32_t, uint32_t, std::array<int, 2>, float gain = 1, uint32_t workers = 0,
                              const BudgetHeatmap* map = nullptr, DebugInspectionCache* cache = nullptr);
Frame budgetDebugImage(const BudgetHeatmap&, int, std::string& legend);
std::string budgetCellInfo(const BudgetHeatmap&, uint32_t, uint32_t);
struct FlatAllocationSavings {
    double referenceBits = 0, currentBits = 0, areaShare = 0;
    double referenceOtherBits = 0, currentOtherBits = 0;
};
// Diagnostic counterfactual. Both maps cover the same source image; working
// resolutions may differ. Compare the SAME white mask from the current map.
// Bits are footprint-distributed estimates, not separately stored flat data.
FlatAllocationSavings compareFlatAllocation(const BudgetHeatmap& current, const BudgetHeatmap& reference);
std::string flatAllocationSavingsText(const FlatAllocationSavings&, bool extraSavings = false);
} // namespace fajk
