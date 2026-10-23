# Fajk Studio R500.1

Windows x64 — verze **0.11.500.1**, checkpoint z 23. října 2026.

Stáhněte [nejnovější vydání](https://github.com/FajkPes/Fajk-Studio/releases/latest), rozbalte ZIP a spusťte **R500/Spustit Fajk Studio.cmd**. Balík obsahuje GUI, CLI, dekódovací SDK a potřebný Windows App SDK runtime.

Tento nový repozitář obsahuje zmrazené zdroje R500.1 odpovídající vydanému checkpointu. Experimentální R510 ani vývojová pohybová predikce nejsou součástí tohoto vydání.

[Změny a dobové výsledky ověření](dev/diagnostics/R500-P1-20261023/RESULTS.md). Zdrojový archiv i binární balík byly před publikací ověřeny proti zachovanému SHA-256 manifestu. Nové funkční ověření publikovaného CLI je uvedeno v poznámkách vydání.

## Dokumentace checkpointu

# Fajk Studio R500

Aktuální sestava **0.11.500.1** je v `release/R500`. Společná nabídka
**Enkodér a dekodér** nastaví CPU/GPU pro kompresi, náhled i přehrávání.
**Fast / More quality** zůstává samostatnou volbou. Adaptivní kvantizace
a ochrana textu jsou výchozí vypnuté; oba přepínače se nadále ukládají.
Při prvním vytvoření nastavení R500 se tyto dvě volby vypnou i při převzetí
starého INI; další publikování už vaše změny nepřepisuje.
CLI přepíná AQ pomocí `--enable-aq` / `--no-aq`; původní `--aq N` nadále
nastavuje její sílu a nezapíná hlavní přepínač.

Oprava .1 zrychluje porovnávání podobných oblastí celočíselným SSE2 bez
změny prahu nebo výsledných paketů. SDK při výstupu s výplní řádků recykluje
pomocný souvislý obraz do 64 MiB na handle; souvislý výstup nadále dekóduje
přímo do bufferu volajícího. Nativní testy DLL běží také pod AddressSanitizerem.
[Ověření a měření opravy .1](dev/diagnostics/R500-P1-20261023/RESULTS.md).

V sekci Video je výchozí **Každý snímek zvlášť (původní)**. Volitelná
mezisnímková komprese používá přímé odkazy na keyframe, bez řetězení rozdílů.
Adaptivní interval keyframu je 120 uložených snímků; 0 vypne věkový limit.
Při toleranci 0 jsou rozdíly přesné vůči zdrojovému keyframu. Shodné snímky
a změny nejvýše 1 % pixelů mohou interval prodlužovat.

Tolerance podobnosti 1–16 dovolí převzít blok 16×16 pouze tehdy, když
rozdíl **každého RGB kanálu každého pixelu** nepřekročí nastavený práh;
alfa musí být shodná. Jde o úrovně 0–255, nikoli procenta. Přibližně shodné
bloky mají limit držení 1–120 uložených snímků, výchozí 10. Změněné oblasti
se uloží přesnými číselnými korekcemi vůči dekódovanému keyframu.
Velká změna nebo příliš drahá korekce použije celý původní snímek. Fast
navíc omezuje rozsah pokusu podle rozpočtu bajtů a ukončí zjevně drahý
pokus podle prvních osmi zakódovaných bloků; More quality může zkoušet déle.
Prototypy se ztrátovou kompresí oddělených oblastí, které tvořily mřížku
a barevné duchy, nejsou součástí R500. Při pohybu kamery může být výsledkem
samá nezávislá komprese a časová režie navíc; pohybová kompenzace není hotová.

Finální ostření 0–100 % běží jednou po úplné rekonstrukci, včetně temporal
složení. Síla neslábne podle počtu bajtů; kontury zůstávají aktivní i nad
80/150 kB. Ostření 0 % je bez průchodu navíc. Maximální síla nemusí zlepšit
percepční skóre. Nabídka **Finální ostření** pod obrazem mění pouze zobrazení.

Pro video s novými číselnými korekcemi (paket 133) použijte SDK/dekodér R500.
Přesné zdrojové rozdíly (132) vyžadují alespoň 0.11.490.2. Staré intra soubory
zůstávají podporované; C ABI je stále 1.
[Ověření, měření a omezení R500](dev/diagnostics/Temporal-20261023/RESULTS.md).

**R500 GPU:** společná nabídka Enkodér a dekodér volí `R500 GPU`
také pro náhled a přehrávání. Obě cesty používají současný
formát R470. Výchozí CPU zůstává zachované; GPU je volitelná a při
nedostupnosti karty či nepodporované operaci použije CPU.
GPU výpočty jsou nová implementace současného R470, nikoli starý GPU experiment.
CLI přijímá `--gpu` / `--cpu`, SDK má `fajk_set_backend`.
[Měření, kvalita a omezení GPU](dev/diagnostics/R470-GPU-20261021/RESULTS.md).
[Navazující optimalizace CPU/GPU z 22. října](dev/diagnostics/R470-GPU-20261022/RESULTS.md)
zlepšují rozdělení práce enkodéru a přesouvají další dočištění na GPU při
rozpočtu 1–2 CPU vláken. Při vyšším počtu vláken zůstává pro tento filtr
rychlejší AVX2 cesta; není nutné nic ručně přepínat.

R480 přidává rychlejší RDO ceny enkodéru a opětovné použití intervalů ochrany
textu při decode. Pakety zůstávají kompatibilní s R470. Výsledky a přesná
regrese: [R480](dev/diagnostics/R480-20261022/RESULTS.md).

R490 navazuje GPU výpočtem barevných statistik, detailové chroma korekce
a integrace chroma modelu přímo nad GPU barevnými rovinami. Zmenšený
obrázek se už zbytečně nenahrává zpět. Finální hranové zvětšení dekodéru
využije GPU při 1–2 CPU vláknech, kdy má přínos včetně přenosů.
Navazující optimalizace uchovává i GPU barevné roviny: příprava 4:2:0,
chroma predikce a první tři úrovně dopředného waveletu mohou běžet přímo
nad nimi, bez uploadu CPU float rovin. Zbývající 1/64 plochy dokončí CPU
kvůli přesné shodě skalárního zaokrouhlení. CPU RDO sdílí nezměněné kontextové ceny mezi kandidáty,
počítá jen potřebné členy Gram matice a vektorizuje aktuální osmipixelové
bloky. Při vypnuté ochraně textu a výstupu do 4 Mpx může GPU spojit kontury a zvětšení bez
mezilehlého stažení obrazu. Výběr cest respektuje kvalitu a podporu zařízení.
[Výsledky R490](dev/diagnostics/R490-20261022/RESULTS.md).

Sestava R490 **0.11.490.1** přidává nezávislou volbu **More quality / Fast**
pro fotografie i video. More quality zachovává ověřenou kvalitní R490;
Fast používá levnější hledání místní přesnosti, vynechá drahé RDO a syntézní
pilot a méně dolaďuje zaplnění paketu. Limit velikosti, AQ, barevné rozlišení
a rekonstrukční ovladače zůstávají samostatné. Fast může při stejné velikosti
ztratit jemné detaily; kvalita závisí na obsahu. CLI: `--fast` nebo
`--more-quality` spolu s `--gpu` či `--cpu`. Starší INI vybere More quality.
[Měření Fast / More quality](dev/diagnostics/R490-20261022/FAST-RESULTS.md).

Spuštění: **Spustit Fajk Studio.cmd** → `release/R500/app/FajkStudio.exe`.
`Build.bat` sestaví, otestuje a publikuje aktuální R500; `Test.bat` provede
kontrolu bez publikování. Zdroje jsou v `dev/src`, SDK v `dev/SDK`.

Adaptivní kvantizace je ve výchozím nastavení vypnutá. Přepínač pod kvalitou dovoluje
skutečné srovnání AQ zapnuto/vypnuto, nezávisle na vyhlazení. Při nízkém toku
vybírá AQ místní kroky v menších blocích a rozděluje přesnost podle zdroje.

Rozdělení rozpočtu dat má tři ovladače:
- **Kontrast a tolerance ploch** (výchozí 50 %): při 100 % používá nízký tok dvě krajní
  rozhodovací ceny — chráněné detaily a silně penalizované ostatní oblasti.
- **Síla úspory v plochách** (výchozí 0 %): dodatečné potlačení detailu uvnitř způsobilých
  ploch. 0 % ponechá základní AQ; 50 / 100 % zvyšuje jejich cenu až 8× / 64×.
  Vyšší hodnota záměrně odstraňuje texturu a může zjednodušit stínování.

**Práh pro minimální data** (výchozí 0 %) rozšiřuje úspornou skupinu do
smíšených oblastí bez rozpoznané hrany. Při 100 % stačí nejméně 25% hladký
podíl místo 95 %. Přímá hrana a její blízké okolí zůstávají chráněné.

Debug mód vpravo obsahuje **AQ · rozhodnutí o rozpočtu** a **Skutečnou spotřebu
dat**, dále skutečné místní kroky Y/C1/C2 a klasifikaci ploch. Pohledy se
obnovují automaticky. Kolečko nad obrazem zoomuje u kurzoru, nad volbou pohledu
mění pohled. Tlačítka ? vysvětlují původ a cenu dat.

Bitová mapa používá skutečné náklady entropických bloků, přiřazené podle
ponechaných koeficientů. Rozdělení uvnitř bloku je prostorový odhad, nikoli
samostatné bajty každého pixelu. Hlavičky/DC a padding se vykazují zvlášť.
Vypnutý Debug nepořizuje mapy ani referenční encode. Do souboru se debug mapy
neukládají; ukládají se potřebné příznaky místní přesnosti a koeficienty.

Používejte přiloženou **SDK R500**. ABI je 1, nativní pakety s místní AQ
používají koeficientový režim 32. Není garantováno, že každý 2K snímek dekóduje
pod 16 ms, zvlášť při souběžné CPU zátěži.

[Architektura](dev/docs/CURRENT.md), [nástroje](dev/tools/README.md),
[výsledky R470](dev/diagnostics/R470-final-20261018/RESULTS.md).
