# Decode: plán využití CPU a GPU směrem k 9 ms

Implementace z 27. 10. 2026: samostatné role GPU bufferů a nativní řídké
uploady jsou hotové ve vývojové větvi. Doklady a závěrečné kontroly jsou v
`../diagnostics/DecodePerformance-20261027/CHECKPOINT.md`; aktuální kvalita
Max v `CURRENT.md`. Tyto dva kroky samy cíl 9 ms nedokládají. Podle nového
požadavku uživatele dostala prioritu také ochrana agresivního rezidua a
oprava barevného šachu; zbývající port vodítek/renderu a asynchronní
pipeline se tím nepovažují za dokončené.

Stav 26. 10. 2026. Cíl je stejný obraz s kratším časem, stabilním přehráváním a nižší prací na snímek. Devět milisekund je ověřovaný cíl, nikoli výsledek nebo záruka odvozená z procent vytížení.

## Co už máme ověřené

Podklady: `../diagnostics/HardwareUtilization-20261026/EVIDENCE.md` a `results.json`. Vydaná R560.1 zůstává beze změny; tato etapa doplnila diagnostiku, profilovala skutečný soubor a připravila implementační pořadí.

- Hardware je Ryzen 5 5600X, 6 fyzických jader / 12 logických procesorů, a RTX 3070. Dvanáct logických procesorů nemá dvojnásobný výpočetní výkon šesti fyzických jader.
- Skutečný soubor ze screenshotu má 1113 snímků, 102 nezávislých snímků a 1011 koeficientových paketů. Cesta Max s pohybem má jiné brzdy a musí mít vlastní kontrolní sadu.
- Po vypnutí hry jsou dva podrobné profily kolem 16,7–16,9 ms/snímek. Čtení paketu je přibližně 0,09 ms. Pro přesné hodnoty a rozdílné počty výskytů použít EVIDENCE, ne součet vnořených průměrů.
- Hlavní práce: obnova aktuálních koeficientů kolem 3 ms, převod detailních koeficientů přibližně 1,2 ms, nativní GPU cesta kolem 3,3 ms, CPU vodítka filtru kolem 3 ms, CPU aplikace filtru kolem 1,5 ms, GPU kontury se zvětšením kolem 3,9 ms.
- Přesun dat požadovaný aplikací je kolem 50 MB/snímek. To není měření skutečné propustnosti PCIe.
- Čítače potvrdily 4517 vytvoření GPU bufferů, součet jejich velikostí 54,5 GB za klip. Jde o kumulované alokace; současně tolik paměti obsazeno není.
- GPU koeficientová/nativní cesta nevyužila žádný řídký upload, přestože pomocná implementace již existuje pro pohybový dekodér.
- Konturové shadery H/V mají skupiny 16 vláken a zhruba 28 KiB sdílených polí. Změnu jejich obsazení je potřeba změřit samostatně.
- V tomto jednovláknovém řetězci je čekání na globální GPU mutex kolem 0,0002 ms. Sériové rozbalení malých nativních metadat trvá kolem 0,004 ms. Prioritu mají jiné úseky.

## Oddělené cílové metriky

1. **Latence SDK do CPU RGBA:** začátek čtení paketu až hotové stejné bajty ve výstupním bufferu, včetně filtrů a readbacku. Cíl 9 ms má smysl porovnávat se stejným výstupním kontraktem.
2. **Latence UI:** požadavek na snímek až jeho prezentace, včetně případné fronty, konverze BGRA, XAML a kompozitoru. Vyhodnocovat také seek a čas prvního snímku.
3. **Propustnost:** interval mezi dokončenými snímky v ustáleném proudu. Překrytí CPU a GPU může tento interval zkrátit bez stejného zkrácení latence jednoho snímku.
4. **Práce a stabilita:** CPU ms/snímek, GPU čas kernelů, p50/p95/p99, vynechané snímky, velikost front, špičky RAM/VRAM, počet přenosů a alokací.

Příklad významu překrytí, nikoli naměřený výsledek: při 7 ms CPU a 8 ms GPU může být interval proudu blízko 8 ms, zatímco samostatný snímek stále potřebuje přibližně 15 ms. Obě čísla musí zůstat v reportu.

## 0. Základ měření — částečně hotovo

Hotové jsou oddělené časové značky pro koeficientové reference, aktuální pásma, sestavení skeletonu, GPU submit, čekání, readback a zámky; také čtení paketu, end-to-end profil, přenosové a alokační čítače. Prošlo 10/10 sad.

Doplnit GPU timestamp/disjoint dotazy po vzorkovaných snímcích; výsledky číst až po dokončení existující GPU práce, s odloženým sběrem. Čítače nesmějí zavádět novou synchronizaci do běžného decode. Stávající `gpu.*` časy jsou CPU intervaly a neznamenají čistý čas GPU kernelu.

Doplnit účast a práci jednotlivých CPU workerů, dobu čekání ve frontě a skutečný počet nových D3D zdrojů podle role. Zvlášť měřit nativní decode, filtry, případnou práci pro originál/audio a UI prezentaci. Baseline: plné průchody po zahřátí, střídavé pořadí variant, alespoň dva běhy, zvlášť studený start. Běhy s hrou jsou v diagnostice označené a vyřazené.

## 1. Stabilní GPU buffery — první oprava

`Engine::ensure` znovu vytváří buffer při změně přesné velikosti. `out/outRead` se v nativním decode a ve zvětšení střídají mezi dvěma rozlišeními; konturová cesta navíc přehazuje `out` s `contourResult`. Paměť se tak alokuje i při stabilním videu.

- Oddělit nativní výstup/readback od zobrazovacího výstupu/readbacku. Pro mezilehlé kontury držet vlastní stabilní roli.
- Zachovat přesný počet platných prvků. Pouhé zvětšení kapacity nestačí, pokud shader nebo CopyResource předpokládá shodnou délku prostředků.
- Při proměnlivé geometrii použít malý, paměťově omezený cache podle rozměrů, stride a způsobu použití. Každý rozpracovaný snímek musí vlastnit své prostředky do dokončení fence.
- Ověřit prokládání dvou rozlišení, selhání alokace, změnu zdroje a device loss. Při stabilní geometrii po zahřátí požadovat nulové další alokace v pravidelné GPU cestě.

Výstup: izolovaný patch se shodnými pixely a reportem počtu alokací, CPU submit času i p95. Úsporu zatím neznáme; 54,5 GB kumulovaných alokací samo o sobě neurčuje počet ušetřených ms.

## 2. Menší koeficientová data a rychlejší CPU obnova

Nejdřív rozšířit existující přesný řídký upload do `decodeNative`, s původní GPU inverzí pro tuto cestu. Všechny float bity včetně záporné nuly zůstanou zachované. Hustá data používají přímý přenos. Měřit i náklady skenování/balení a zachovat omezený staging.

Následně:

- Vektorizovat předpověď a součet kvantovaných koeficientů po osmi hodnotách přes AVX2. Zachovat float násobení, saturaci, half-away zaokrouhlení, validaci přetečení a skalární konce.
- Přeskočit bloky, kde je současné reziduum i předchozí koeficient nulový, po ověření přesné ekvivalence. Velká pásma rozdělit pro nezávislé dekvantování; závislé entropické/predikční kroky zůstanou ve svém správném pořadí.
- Doplnit omezenou cache původního koeficientového rootu vedle poslední reference. Současná cache drží jedinou referenci; profil ukazuje i dodatečné obnovy předků. Klíč musí mít spolehlivou identitu a obsah, rozměry i parametry.
- Dlouhodobě předat GPU nenulové kvantované hodnoty, jejich polohy a přesné parametry pásem. GPU provede dekvantování/scatter, čímž zmizí další velké CPU pole a jeho průchod. Tuto cestu nejprve porovnat bitově na float koeficientech i výsledném obrazu.
- Skeleton přes entropické překódování je potvrzená práce v dekodéru, ale zatím jen kolem 0,125 ms/výskyt. Přímý strukturovaný vstup do nativního decode zavést při refaktoringu, nepovažovat ho za hlavní zdroj dvojnásobného zrychlení.

## 3. Lepší využití GPU při konturách

U H/V kernelů je 16 vláken ve skupině slabý kandidát pro hardware s 32vláknovými warpy. Změna `numthreads` bez nového rozdělení práce by ale nestačila a rozšíření dlaždice by mohlo překročit limit sdílené paměti.

Prototyp: ponechat geometrii a sdílená data, zpracovat 16 linek × 3 barevné kanály pomocí 48 aktivních vláken ve skupině 64. Načtení a uložení dlaždice rozdělit mezi všechna vlákna; váhy sdílet, skalární rekurenci každého kanálu nechat ve stejném pořadí. Rozvržení sdílených polí ověřit proti bank conflicts a registrům.

Všechna vlákna musí správně procházet bariérami i na okrajích. Zápis float4 výstupu provést po dokončení kanálů tak, aby nevznikly souběžné zápisy stejného prvku. Kromě 64 vyzkoušet i jiné vhodné konfigurace, měřit skutečný GPU čas a výsledné pixely. Přepis na paralelní prefix scan je samostatný pozdější experiment, protože mění pořadí floating-point operací.

## 4. Souvislý GPU řetězec filtrů

Dnes se nativní obraz stáhne do CPU, CPU vytvoří vodítka a upraví pixely, obraz se znovu nahraje pro GPU kontury/zvětšení a znovu stáhne. CPU část kolem 4,5 ms je další hlavní kandidát.

- Připravit GPU variantu tvorby vodítek a jejich aplikace; oddělit lokální odhady, případnou regularizaci a render.
- Sdílet načtené okolí mezi vodítky, která nyní analyzují překrývající se pixely. Zachovat alfa kanál, textovou ochranu, mapy přesných oblastí, hraniční pravidla, pořadí filtrů a FMA/reciprocal chování.
- Udržet nativní RGBA residentní od barevné obnovy přes čištění, kontury a upscale až po finální ostření. Pro CPU SDK stáhnout až poslední výstup.
- Částečný přesun jen jednoho filtru přijmout pouze při měřitelném přínosu včetně přenosu vodítek. Přidání GPU průchodu může jinak cestu prodloužit.
- Začít přesnou variantou. Jakákoli odchylka proti CPU orákulu vyžaduje vyhodnocení hran, barev, průhlednosti a časového kolísání; neprohlásit horší obraz za rychlejší ekvivalent.

## 5. Překrývání snímků a skutečně společné plánování CPU/GPU

UI má jeden postupně dekódující thread a frontu nejvýše tří hotových snímků. Samotný prefetch hotových snímků ještě nepřekrývá přípravu dalších koeficientů s GPU prací současného snímku.

Zavést malý okruh 2–3 rozpracovaných úloh:

1. CPU načte/ověří paket a připraví další koeficienty.
2. Jeden vlastník D3D11 immediate contextu zadá GPU práci.
3. CPU připravuje další úlohu, zatímco GPU dokončuje předchozí.
4. Dokončené výstupy se předávají UI/SDK v pořadí PTS, s explicitní dobou života prostředků.

Koeficientová reference může být pro příští snímek připravena před dokončením zobrazení současného snímku. U Max musí pohyb čekat na kanonickou rekonstruovanou referenci; entropické rozbalení budoucího paketu čekat nemusí. Kanonická reference a zobrazovací filtr musejí zůstat samostatné, aby se nevrátili duchové a drift.

GPU mutex prostě neodstraňovat: immediate context ani sdílené buffery nejsou bezpečné pro nekontrolované souběžné volání. Použít frontu a zdroje vlastněné úlohou. Globálně rozdělit CPU budget mezi přípravu, filtry a UI, aby dva snímky každý nevytvářely plný dvanáctivláknový nárok. Změna worker budgetu nesmí přepínat mezi různě zaokrouhlujícími kvalitativními cestami.

Seek, nový soubor, změna filtrů a rušení úlohy mají generační identitu. Starý snímek se nesmí prezentovat; jeho prostředky se přitom nemohou recyklovat před GPU fence. Root cache má pomáhat i při seeku a dlouhých exact holdech.

## 6. Přímé GPU zobrazení a UI

Zachovat CPU RGBA SDK pro export a externí volající. Pro UI přidat samostatnou cestu do GPU textury/SwapChainPanel: GPU výstup v potřebném barevném a alfa formátu, bez běžného readbacku a následného `displayPixels`/WriteableBitmap uploadu.

Tento výstup má jiný kontrakt než hotový CPU buffer, proto se musí reportovat odděleně. Pro PNG, clipboard a porovnání zajistit explicitní download. Ošetřit resize, DPI, zoom, zařízení ztracené během přehrávání, pozastavení a návrat na CPU renderer.

Oddělit prezentaci podle obnovování obrazovky a PTS od 15ms DispatcherTimeru pro obsluhu UI. Časovač dnes může ovlivňovat propustnost prezentace, nikoli samotné změřené SDK decode. Reálné přehrávání má respektovat FPS videa a audio hodiny; vytížení při běžném 1× přehrávání bude proto nižší než při neomezeném benchmarku.

Prověřit i originál připojený kvůli zvuku: frame-server mód potlačuje vykreslení videa, sám o sobě nedokládá vypnutí jeho dekódování. Porovnat přehrávání se zvukem/bez něj a případně použít audio-only cestu. Graf Video Decode ze screenshotu není důkaz, že tuto jednotku využívá vlastní FAJK bitstream.

## 7. CPU plánovač, různé stroje a encode

Výsledky 6/10/12 vláken ukazují omezené škálování; agresivní přidávání threadů nevyřeší přenosy a závislosti. Změřit délky úloh, rozběh Windows threadpoolu, čekání a rozdělení práce. Ověřit přínos předehřátých workerů, adaptivních chunků a dynamického sdílení rozpočtu mezi nestejně velkými pásmy. Zachovat paměťové limity a spící idle workery.

Testovat také méně vláken, CPU režim, GPU bez potřebné double podpory, malé obrázky, 4K, proměnlivou geometrii a více handle současně. Fixní volba pro 1440p RTX 3070 není univerzální optimum.

U encode navázat na už existující GPU source cache a fused forward cestu. Ověřit využití přesné GPU inverze pro kanonickou obnovu referencí enkodéru, CPU balení/entropii překrývat s nezávislou GPU přípravou a zachovat pořadí závislých Max snímků. Samostatné GPU generování nového formátu nebo nové entropické partitions řešit až po změření zbylého bottlenecku a ceny v kompresi.

## Kritéria přijetí a pořadí práce

Pořadí: stabilní buffery → sparse nativní upload → AVX2/rozvržení koeficientů a cache → H/V kernel → souvislé GPU filtry → asynchronní okruh → přímá GPU prezentace. Jednotlivé nezávislé změny držet měřitelné, kombinace znovu otestovat.

Pracovní cíle dalších etap jsou nejdřív pod 15 ms, následně pod 12 ms a nakonec prověřit 9 ms. Nejde o předpověď výnosu jednotlivých patchů. CPU-buffer latence, UI latence a interval proudu mají vlastní výsledky; sliby z dílčích úspor se nesčítají.

Před zapnutím každé změny:

- shodné pixely pro změny datové cesty a pořadí práce; CPU/GPU matematické orákulum pro nové kernely;
- celý skutečný herní soubor, Max 30/80 kB se silou 6/14, intra, koeficienty, exact hold a čerstvé i opakované seeky;
- detailní hrany, text, obloha, jemná tráva, pohyb, průhlednost, nízký rozpočet, neúplné dlaždice a změna rozlišení;
- žádný růst časového šumu, barevných stop, chybné reference nebo viditelných linek; pro změněný filtr vedle obrazových metrik také sousední snímky/ROI;
- guard buffery, malformed input, ASAN, změna backendu/nastavení, rušení, device loss a omezená RAM/VRAM;
- párové benchmarky bez další naší zátěže, studený start zvlášť, stálé nastavení a stav vydaných binárek doložený hashem;
- rychlejší p50 i přijatelný p95/p99, stabilní spotřeba paměti, rychlý seek a funkční UI. Počet procent vytížení není přijímací kritérium.

## Dokumentace k hardwaru a API

- [Microsoft: GPU ve Správci úloh](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/) — souhrn ukazuje nejvytíženější engine; není to podíl využitých aritmetických jednotek.
- [Microsoft: D3D11 timestamp/disjoint](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query) — přesná pravidla GPU časových dotazů.
- [Microsoft: multithreading D3D11](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro) — jeden immediate context vyžaduje řízené vlastnictví/synchronizaci.
- [NVIDIA: warpy a SIMT](https://docs.nvidia.com/cuda/archive/13.2.2/cuda-programming-guide/01-introduction/programming-model.html) — důvod prověřit malé skupiny; skutečný přínos HLSL variant musí potvrdit GPU měření.
- [Microsoft: video frame-server mód](https://learn.microsoft.com/en-us/uwp/api/windows.media.playback.mediaplayer.isvideoframeserverenabled?view=winrt-26100) — vypíná prezentaci videa, poskytuje dekódované snímky aplikaci.
