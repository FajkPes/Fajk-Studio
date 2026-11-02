> Windows balík R600.4 stáhnete na [stránce vydání](https://github.com/FajkPes/Fajk-Studio/releases/latest). Starší verze R500.1 zůstává v Releases.

# Fajk Studio — R600.4 (0.11.600.4)

R600.4 přidává **Max komprese · Fast**, samostatný režim vedle Fast a More
quality. Má rychlé hledání Fast, omezenou korekci silných barevných detailů
před kvantizací a při vyšším rozpočtu větší pracovní rozlišení s novým
uniformním kontextovým modelem. AQ se nezapíná a výběr barevných složek
se nemění. CLI: `--fast-max`. Mezisnímková metoda je nezávislá volba.

Na třech 1440p herních snímcích při limitu 80 kB klesla proti Fast chyba
červených oblastí o 3,6–6,5 % a chyba hran o 8,5–11,6 %. Při 30 kB jsou
zisky menší a ne všechny metriky se zlepšují. Dva párové exporty 32snímkového
1440p klipu byly v průměru o 5,2 % rychlejší; sekvenční decode vzrostl
z 10,32 na 11,62 ms/snímek. Nejde o záruku dvojnásobné kvality.
Podrobné výsledky a omezení jsou v [aktuálním záznamu](dev/docs/CURRENT.md).
Hlavní spouštěč používá R600.4, nastavení se převezme z R600.3.

R600.3 ukazuje nastavenou a výslednou sílu Skirt/deringu. Automatika snižuje
nastavenou sílu lineárně mezi kvalitou 10 a 50 a zohledňuje hustotu dat.
Po jejím vypnutí platí ruční síla bez obou útlumů. Posuvník si svou hodnotu
pamatuje; po výpočtu náhledu se výsledná síla počítá ze skutečného paketu.

Kontury a tvary mají toleranci a minimální měřítko 25–200 %, obě výchozí
100 %. Při 100 % zachovávají předchozí rozpoznávání. Nižší tolerance je
přísnější, vyšší dovoluje méně pravidelnou konturu; menší měřítko hledá
drobnější tvary. Měřítko je relativní velikost analyzovaného okolí,
nikoli ostrý pixelový limit. Nastavení se ukládá do souboru; vlastní volby
přehrávejte dodaným SDK R600.3 nebo novějším.

R600.2 rozšiřuje finální ostření na 0–200 % v UI i SDK. Původních 100 %
zachovává dosavadní sílu. CPU ostření používá krátké řádkové buffery a
předpočítaný jas; na testovaném 1440p obrazu s 10 workery kleslo z 1,90
na 0,93 ms. GPU kontury rozdělují barevné složky mezi více vláken a
zachovávají přesné pixely. Zrychlení celého decode je menší: CPU 80k
23,03 → 21,79 ms/snímek, GPU se dvěma větvemi přibližně beze změny.
Nastavení R600.2 se převzalo z R600.1; původní vydání zůstávají zachovaná.

R600.1 zobrazuje skutečný průměr aktivního decode na hotový snímek a rychlost
dekodéru ve snímcích/s, odděleně od latence úlohy a převzetí z fronty.
Souběh se započítá jednou a čekání na plnou frontu je vynechané.
Opravené jsou i názvy CPU/GPU v nabídce.

R600 spojuje renderer základního čištění, kontury, zvětšení a ostření na GPU.
Omezená cache nativního GPU obrazu odstraňuje opakovaný upload; při evikci
zůstává bezpečná cesta z RAM. CPU nadále připravuje vodicí data pro čištění.
Max porovnává přesné alternativy z nedávné reference a původního keyframu.
Přesný režim zachovává kratší řetězce z nedávných referencí; vynucené návraty
ke starému keyframu byly při měření horší a nepoužívají se.
CPU Max neopakuje kanonickou rekonstrukci.
Měření a ověření obrazu jsou v [aktuálním záznamu](dev/docs/CURRENT.md).

R560.1 přidává rychlou AVX2 pohybovou predikci pro CPU a spojené GPU dekódování: predikci, přesnou inverzi, obnovu barev a referenční filtr. Mezilehlý předpovězený obraz se nevrací do RAM a řídké koeficienty se přenášejí bez zbytečných nul, s přesným zachováním hodnot. Při nepodporované GPU operaci funguje CPU fallback se stejným obrazem. Opravena je také obnova reference při změně rozlišení a zacházení s pamětí volitelné cache. Původní R560 zůstává v samostatné složce.

Předchozí R560 zrychluje obnovu barevného rezidua přes AVX2, přeskakuje nepoužité výpočty při úplném převzetí obrazu a opakovaně využívá přesně shodný keyframe. Optimalizace zachovávají pixely i zakódovaná data R550; síla komprese se nezvyšuje. Cache jednoho původního keyframu je omezena na 32 MiB na volající vlákno včetně jeho zakódovaných dat. Nativní výstup do bufferu odstraňuje jednu alokaci a celou kopii obrazu. Nastavení nového balíčku se převezme z R550, pokud dosud neexistuje nastavení R560.

Předchozí R550 upravilo volitelnou pohybovou predikci **Max komprese**: opravuje přenos barevných chyb, tlumí hromadění jasové chyby a omezuje barevné zrnění. Běžné nastavení 0–6 má jemnější kvantizaci; agresivní 12–16 zůstává volitelné.
V sekci Video zapněte Mezisnímkovou kompresi a vyberte metodu:

- **Koeficienty – přesně stejný obraz**: bezztrátově překóduje koeficienty
  samostatně zakódovaných snímků. Zachová jejich pixely i původní časové
  kolísání kresby. CLI: `--temporal-coefficients`.
- **Oblasti – nastavitelná podobnost**: přebírá podobné části na stejném
  místě a ukládá číselné korekce změněných bloků. Má vlastní toleranci RGB
  a limit držení podobných oblastí. CLI: `--temporal`.
- **Max komprese – pohyb a tolerance (experimentální)**: hledá posun
  po čtvrtinách pixelu a místní afinní změnu (otočení/měřítko/smyk),
  předpoví obraz z rekonstruované reference a uloží kvantované reziduum.
  Začněte silou 6; vyšší hodnoty až 16 mohou ušetřit více za cenu detailů.
  Nula je nejjemnější ztrátové nastavení, nikoli bezztrátový režim.
  CLI: `--temporal-max --similar-tolerance 6`.

Výchozí **Každý snímek zvlášť (původní)** zůstává dostupné; CLI
`--intra-only`. Síla Max komprese a tolerance metody Oblasti se ukládají
odděleně, takže přepnutí metody nemění nastavení té druhé. Fast / More quality
je samostatná volba. AQ a ochrana textu jsou výchozí vypnuté.

Nové koeficientové a pohybové video používá kontejner v3, pakety 136/137.
Export má nejvýše 16 závislostí. Při změnách platí strop reference 5 sekund
podle PTS i případný kratší interval zvolený uživatelem. Přesně statická část
používá osmibajtové přímé odkazy, bez zbytečných keyframů a rostoucího řetězce.
Max může kratší řetězec restartovat ze stejného keyframu; drahá predikce se
nahradí samostatným snímkem. Libovolné přeskakování podporuje dodané SDK.

GPU volba je společná pro enkodér, náhled a přehrávání. V Max režimu GPU
navíc hledá pohyb a afinní parametry a spojuje přípravu barevného rezidua
s dopřednou transformací bez mezilehlého přenosu float rovin. Enkodér využívá
vybrané koeficienty přímo, nedekóduje vlastní čerstvě zapsanou entropii.
Příliš velký kandidát vynechá inverzi a RGB rekonstrukci. Entropie, volba
režimů a obnova enkodérové reference zůstávají na CPU. GPU dekodér využívá
přesnou inverzi a navazující zpracování na zařízení; CPU fallback zachová
stejnou referenci i po přeskoku nebo změně filtrů.

Max není zárukou stejné kvality ani rychlejšího encode než samostatné snímky.
Úspora závisí na obsahu a síle. Přibližné ostré masky 8×8 se v něm nepoužívají;
opravy se počítají průběžně. Kresba reference může zachovat omezenou část své
chyby, aby se zbytečně nepřekreslovala při malých změnách. Filtry zobrazení
se nikdy nevracejí do predikční reference. Nelze slíbit úplné odstranění
blikání, zejména u samostatně kódovaných snímků s malým rozpočtem.

Použijte dodané SDK R560 (C ABI 1); nové Max reziduum 134/15 vyžaduje tuto DLL. R500 nové v3 soubory nepřečte. Staré
intra soubory i kontejner v1/v2 zůstávají podporované. Původní vydání R550
je zachováno v samostatné složce; nastavení R560 je mimo nahrazovanou složku app.

## Informace o předchozích vydáních

Společná nabídka
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

**R510 GPU:** společná nabídka Enkodér a dekodér volí `R510 GPU`
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
`Build.bat` sestaví, otestuje a publikuje aktuální R510; `Test.bat` provede
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

Používejte přiloženou **SDK R510**. ABI je 1, nativní pakety s místní AQ
používají koeficientový režim 32. Není garantováno, že každý 2K snímek dekóduje
pod 16 ms, zvlášť při souběžné CPU zátěži.

[Architektura](dev/docs/CURRENT.md), [nástroje](dev/tools/README.md),
[výsledky R470](dev/diagnostics/R470-final-20261018/RESULTS.md).
