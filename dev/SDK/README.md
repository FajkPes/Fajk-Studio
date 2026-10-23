# Fajk decoder SDK R500 (0.11.500.1, C ABI 1)

Oprava .1 recykluje souvislý pomocný RGBA obraz při stride > width*4,
takže ho nemusí pro každý snímek alokovat a nulovat. Cache má nejvýše 64 MiB
na handle a uvolní se při fajk_close; větší obrazy používají dočasný buffer.
Při stride == width*4 se stále dekóduje přímo do bufferu volajícího.
Pixely, padding, ochrana velikosti bufferu i C ABI se nemění. Nativní
fajk_sdk_tests ověřují skutečnou DLL v Release i pod AddressSanitizerem,
včetně změn alfa, korekcí, postfiltrů a opakovaného použití pomocného obrazu.

R500 podporuje nové přesné číselné korekce (paket 133, režim 4, bloky 16×16)
v kontejneru v2. Každý odkaz vede přímo na nezávislý keyframe; náhodný seek
nevyžaduje dekódovat předchozí delty. Starší DLL tyto korekce nepodporují.
Přesné zdrojové rozdíly 132 i původní intra soubory jsou dále podporované.
Experimentální ztrátové oblasti (soukromé režimy 133/1–3) se odmítnou.
Pro predikci 133 se reference rekonstruuje s deblocking=100, sharpening=0;
uživatelské finální ostření se aplikuje až na úplně složený snímek.
Na stejném backendu a při stejném rozpočtu vláken obnoví změněné bloky
přesně zdrojové pixely. CPU/GPU se mohou lišit zaokrouhlením reference.
Převzaté bloky mají chybu původního keyframu a případnou povolenou toleranci;
celý ztrátový soubor tím není bezeztrátový.

Čtení nezávislého keyframu i následujícího rozdílu používá tutéž cache
dekódované reference do 64 MiB. Změna backendu, vláken či postfiltrů cache
invaliduje. R500 prošlo 216 kontrolami SDK/CLI na třech temporal souborech,
včetně číselných korekcí, více keyframů, náhodného seeku, 1/2/10 vláken,
CPU/GPU, změn postfiltrů, souběžných handles a ochrany výplně bufferu.

Experimentální sestava **0.11.490.2 Temporal** navíc čte kontejner v2
s přímými odkazy na keyframe a přesnými zdrojovými rozdíly bloků. C ABI
zůstává 1 a `fajk_decode_frame` nadále přijímá libovolný index bez předchozího
sekvenčního decode. SDK samo načte referenci a cache ji invaliduje při změně
backendu/vláken/postfiltrů. Pro nové temporal soubory použijte DLL .2 z tohoto
experimentálního balíčku; stará DLL .1 je odmítne. Staré intra soubory fungují dál.

**0.11.490.3** mění zobrazení: ostření se provede jednou na úplně hotovém
snímku, včetně temporal složení, a není tlumené velikostí souboru. Kontury
mají zachovaný účinek nad 80/150 kB. `fajk_set_sharpening(0)` je výchozí
identita; 50/100 lze zvolit podle požadovaného vzhledu. CPU AVX2 a GPU cesta
samotného ostření jsou celočíselné a pixelově shodné. S malým rozpočtem CPU
vláken lze ostření provést na GPU; při větším rozpočtu je rychlejší AVX2.
Intenzita kontur 0 zachová původní základ bez hlavního čištění.

R490 zachovává C ABI 1 i pakety R470/R480. Backend 0 je CPU, 1 GPU.
Soubory vytvořené volitelným encode režimem Fast používají stejnou syntaxi
a lze je dekódovat stejným SDK. Pro dekódování není volba encode rychlosti potřeba.
Při vypnuté ochraně textu a výstupu do 4 Mpx může GPU spojit kontury a finální zvětšení bez
mezilehlého stažení RGBA; původní parametry rekonstrukce se zachovávají.
Při rozpočtu 1–2 CPU vláken může GPU provést také finální hranové zvětšení
obrazu. Souřadnice, ochrana hran a omezení barev odpovídají AVX2 cestě;
izolované zaokrouhlení se může lišit o 1 úroveň. Při větším počtu vláken
zůstává pro tento krok rychlejší AVX2. Výsledky a meze:
`dev/diagnostics/R490-20261022/RESULTS.md`.

## R470 GPU (21. 10. 2026)

Nová DLL přidává `fajk_set_backend(decoder, 1)` pro GPU dekódování;
`0` ponechá výchozí CPU. C ABI zůstává 1, jde o nové volitelné volání.
Obě cesty čtou stejné soubory R470; volba GPU není příznak v souboru.
Použijte přiloženou novou DLL a hlavičku pro toto nové volání.

Direct3D 11 compute provádí CDF 9/7, uložené nativní filtry, CfL a obnovu
barev jako jednu dávku v paměti karty. Zpět do bufferu volajícího jde RGBA8.
Při rozpočtu 1–2 CPU vláken GPU navíc provádí rekurzivní RGB dočištění;
jeho float mezivýsledky zůstávají na kartě. Při vyšším počtu vláken se
pro tento filtr zachovává rychlejší AVX2 cesta. Entropické dekódování,
text guard, další RGB rekonstrukce a některé zvláštní volby zůstávají
na CPU. Malé obrazy, nepodporované kombinace a nedostupná GPU použijí CPU
cestu. Finální ruční ostření již nativní GPU decode nevypíná. Volba je vlastní konkrétnímu
decoder handle; neovlivní jiné handly. Stride a kapacita platí stejně.

GPU používá float32 a stejné algoritmy. Výstup může mít malé zaokrouhlovací
rozdíly oproti CPU, které mohou ovlivnit i následné diskrétní filtry. Není
garantována bitová shoda pixelů mezi CPU a GPU. Bezeztrátové/paletové
soubory nadále procházejí přesnou CPU cestou. Shoda SDK/CLI se ověřuje pro
stejný zvolený backend. Shadery jsou sestavené uvnitř DLL; za běhu se
nekompilují ani nestahují. GPU není nutná k otevření souboru.

Měření a meze: `dev/diagnostics/R470-GPU-20261021/RESULTS.md`.

Aktualizace 22. 10. 2026: vnořené CPU úlohy si dělí limit vláken místo
jeho násobení. Nezávislé handly stále mají samostatné rozpočty; při souběhu
rozdělte celkový požadovaný počet vláken mezi handly na straně volajícího.
GPU context chrání společný mutex. Aktuální DLL prošla 144 kontrolami
SDK/CLI včetně dvou souběžných handlů, backendů CPU/GPU, limitů 1/2/10,
výplně řádků a ochranných bajtů. ABI ani požadovaná velikost bufferu se nemění.
Video měření a audit: `dev/diagnostics/R470-video-20261022/RESULTS.md`
a `PARALLELISM.md` ve stejné složce.

Windows x64. C ABI 1: `decode.h`, `fajk_decode.dll` a importní knihovna
`fajk_decode.lib`. DLL podporuje pouze současnou rodinu komponent R441.
Historické formáty mimo tuto rodinu byly odstraněny.

`fajk_decode_frame` zapisuje RGBA8 do bufferu volajícího. Stride musí být
alespoň width*4 a kapacita alespoň stride*(height-1)+width*4. Výplň řádků
se nepřepisuje. Při chybě funkce vrací -1; podrobnost je ve `fajk_last_error`.
Jeden decoder handle používá vždy jeden volající současně.

Uložené nastavení R441 se používá také přes SDK. Stejná rychlá CPU/AVX2
rekonstrukce jako v aplikaci; není potřeba GPU. Recyklujte RGBA buffer mezi
snímky. Měření do připraveného bufferu nezahrnuje jeho prvotní alokaci.
Některé úklidové filtry slábnou podle hustoty dat; hlavní kontury v R500
zachovávají účinek i nad 150 kB / 2560×1440. Nulová intenzita kontury vypne.
Historická pomalá reference již není požadovaným RGBA výstupem.

R460: ruční uložené doostření není útlumem podle kvality vypnuté.

R461 čte i 4:2:2 (příznak 0x40 v nativním wavelet paketu). Starší DLL tuto novou volbu nepodporují; 4:2:0 a 4:4:4 zůstávají stejné.

R462: rozšířená místní přesnost (koeficientový režim 24) pro nový ovladač
alokace mimo 50 %. Použijte aktuální DLL; starší DLL R461 nové pakety odmítne.
ABI i volání jsou stejné, výchozí pakety při 50 % se nemění.

Opravy auditu R462 (18. 10. 2026): `fajk_set_deblocking` a
`fajk_set_sharpening` se předávají i přes vnořené scaled/flat/CfL obaly.
Výchozích 100/0 zachovává dosavadní obraz; nula vypíná nativní čištění,
nikoli samostatnou rekonstrukci R441 uloženou v souboru. CfL má nadále
svou původní interpolaci.

R465 zahrnuje rychlejší CPU rekonstrukci aktuální vývojové větve. Režim 31
může mít selector 7 pro sedm základních úrovní magnitudy. Použijte DLL tohoto
vydání; starší publikovaná DLL selector 7 nečte. Nový encoder již nepřepíná
na režim 24 podle hodnoty úspory ploch, ale decoder jej nadále podporuje
pro předchozí soubory. Druhý pilot a jeho výběr bez produkce bytů jsou
výhradně v enkodéru a nepřidávají SDK žádný dekódovací průchod.

Finální R465 používá při nízkém toku se zapnutou AQ režim 32 (8×8 místní
koeficientové dlaždice, faktory kroku 0,25 / 0,5 / 1 / 2 / 4). Tato data
vyžadují přiloženou DLL R465; předchozí vydání je odmítnou. ABI je stále 1.
Nové alokační ovladače jsou pouze v enkodéru. Debug mapy ani zdrojové ceny
se v paketu nenacházejí; dekodér čte pouze místní příznaky přesnosti a vybrané
koeficienty. Měření kvality a celého dekódu viz aktuální diagnostika R465.

R470: stejné C ABI 1. Nový build se dodává společně s aplikací a CLI.
Slabé tmavé hrany a alokační ovladače jsou změny enkodéru. Zpřesněné debug
přiřazení nákladů ani kontrola paketových intervalů nevstupují do produkčního
dekódu a v souboru nevytvářejí další kopii obrazových dat.
