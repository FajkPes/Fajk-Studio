# R600.4 — Max komprese založená na Fast, 31. 10. 2026

EncodeSpeed::MaxCompression (hodnota 2), UI Max komprese · Fast, CLI
--fast-max. Zůstávají More quality (0) a Fast (1). Společný fastEncode()
zajišťuje rychlé volby i v řízení velikosti a metodě podobných oblastí.
Nezapíná se drahé RDO, synthesis pilot ani AQ. Přepnutí režimu zneplatní
kompresní náhled; hodnota 2 se ukládá a obnovuje v settings.ini.

Před kvantizací se ve dvou nejjemnějších barevných pásmech aplikuje gain
1 + 0,125 × clamp((abs(value)−4)/12,0,1). Malé koeficienty se nemění;
silná barevná struktura částečně kompenzuje předchozí průměrování chroma.
Jasové předzpracování Fast zůstává. Dekodér nepřidává žádný obrazový pass.
Pracovní rozlišení se při ekvivalentní hustotě nad 40–60 kB / 1440p zvýší
až o 1/16 původního rozměru. Při malém rozpočtu se tato změna neuplatní.

Hustší uniformní data používají model 33: kontexty modelu 32, dlaždice 8×8,
bez mapy lokální přesnosti a bez dodatečného compactSpatialBand překódování.
Při nižší hustotě se zachovává původní kompaktní 23/31. Explicitně zapnutá
AQ používá 32. Model 33 odmítá AQ i nesprávný prefix. Při obnově koeficientové
reference se nepřeznačí nezměněná pásma 33 na model 31; změněná pásma se
znovu zapíší správným modelem. C ABI zůstává 1, nový model vyžaduje DLL R600.4.

Ověřená konečná varianta je h v dev/diagnostics/FastMax-20261031/.
Zamítnuté varianty c–g dokumentují mimo jiné škodlivé zvýšení rozlišení
při 30 kB, nevýhodu univerzálního modelu 33 při nejnižší hustotě a přílišné
přerozdělení bitů na barvy. Tyto změny nejsou součástí výsledné konfigurace.

Párové testy proti Fast, Ryzen 5600X / RTX 3070, 10 workerů, 2560×1440,
4 obrácená pořadí pro fotografie, ostření 50, stejné limity 30/80 kB.
Kvalita a PNG se počítají mimo měřený čas. Červené masky vycházejí pouze
ze zdroje, nezávisle na kandidátovi. Nejde o přesně shodné počty bajtů.

- Tři herní snímky, 80 kB: RGB MSE −7,9 až −10,9 %, červené oblasti
  −3,6 až −6,5 %, hrany −8,5 až −11,6 %. SSIMULACRA2 +2,6 až +11,4 bodu.
- Stejné snímky, 30 kB: RGB MSE −0,7 až −1,8 %, červené oblasti −1,2
  až −2,5 %. SSIMULACRA2 se změnila o přibližně −0,38 až +0,03 bodu.
  Proxy barevného šumu v plochách mírně vzrostlo; nelze slíbit zlepšení
  každé oblasti nebo každé metriky. Syntetické tvary rovněž nemají plošný zisk.
- Dvě obrácená pořadí reálného 32snímkového MP4 exportu při 80 kB:
  Fast 723,2825 ms, Max 685,8165 ms (−5,18 %). Sekvenční decode Fast
  330,219 ms / 32 = 10,3193 ms, Max 371,7315 / 32 = 11,6166 ms (+12,57 %).
  Velikosti 2 526 714 / 2 542 924 B (+0,64 %), oba ve stejném limitu na snímek.
  Export obsahuje skutečné čtení MP4, plánování a zápis; nejde o UI FPS.

Reálné video: 32 snímků × 4 exporty, přesná shoda paketů a časů pipeline,
CPU/GPU cross-decode, cancellation/ochrana existujícího výstupu, sekvenční
seek. Dále 48snímkový export oběma mezisnímkovými metodami (koeficienty,
Max pohyb), oběma backendy v 1/2 větvích; shodné CRC/PTS/duration v SDK
i přeskakování. UI ověřeno na CPU i GPU, přepnutí režimu a jeho obnovení
zachovává přesné pakety a pixely. Release 11/11, plný ASAN 11/11 (205,88 s).
Finální Release po doplnění kontrol neplatné kombinace modelu 33: 11/11
(19,64 s), cílený ASAN core/robustness 2/2 (6,21 s), opakované UI CPU/GPU
a 24 seeků/snímků přes konečnou DLL proti integračním CRC a časům.

# R600.3 — skutečná síla Skirtu a nastavení rozpoznávání tvarů, 31. 10. 2026

UI zachovává surovou nastavenou intenzitu odděleně od intenzity uložené
v paketu. Automatika násobí surovou hodnotu clamp((50−quality)/40,0,1),
zaokrouhlí ji jednou do intenzity wrapperu a dekodér zohlední skutečnou
hustotu uložených dat. Při kvalitě 10 je faktor 1, při 30 faktor 0,5,
při 50 a výše 0. Posuvník ani uložená preference se tím nepřepisují.

Vypnutí automatiky ukládá původní ruční intenzitu a příznak fixedIntensity
0x0004 wrapperu 130. CPU i GPU pak používají stejné míchání bez útlumu
hustotou dat. Výsledné míchání zůstává omezeno na 100 %. AA, Stronger Tangents,
doostření a základní Kontury a tvary jsou samostatná nastavení.

U výsledné síly je před novým náhledem uvedeno Odhad. Použito je dostupné
až pro náhled odpovídající současným kompresním i rekonstrukčním volbám,
se skutečnou hustotou z jeho paketu. Bez známé hustoty je uvedena horní mez.

Reconstruction441.shapes obsahuje toleranci a minimální měřítko, 25–200 %,
výchozí 100/100. Tolerance mění barevnou chybu a geometrickou odchylku
lokálního modelu. Měřítko mění krok široké/jemné analýzy (původně 4/2)
v pracovním obrazu. Nejde o tvrdý limit rozměrů detekovaného objektu.
Při lichém kroku se počátek modelu koriguje o půl pixelu podle skutečných
vzorků. Průhlednost, tenké tahy, síla a volba gridu mají původní ochrany.

Výchozích 100/100 se serializuje jako původní nulové slovo na offsetu 30.
Vlastní hodnoty se uloží do jeho dvou bajtů. Nové volby vyžadují dodanou
DLL R600.3; staré pakety mají původní chování. CPU sestaví vodicí modely
a GPU používá stejné upravené modely ve stávajícím rendereru. Stejná data
používá motion zobrazení i diagnostická mapa. Změna těchto voleb může
znovu použít komprimovaný podklad beze změny entropických dat.

Podrobné protokoly a validace: dev/diagnostics/SkirtControls-20261031/.
Finální Release 11/11 (17,78 s), cílený ASAN reconstruction/GPU 2/2
(43,42 s), 1 200 snímků přesné shody proti R600.2 (CPU/GPU, stride,
seek, PTS/duration), tři UI průchody včetně automatického vypnutí,
ručního režimu a vlastní tolerance/měřítka. GPU oracle ověřuje
216 kombinací rozměrů, vstupů, gridu, síly a nastavení tvarů.

# R600.2 — rychlejší filtry a ostření do 200 %, 31. 10. 2026

Vydaná verze 0.11.600.2 je v release/R600.2. Hlavní spouštěč a dev/SDK
používají tuto verzi; nastavení se převzalo z R600.1. Starší vydání
a archivy zůstávají zachované. Formát paketů a výchozí síla se nemění.

Finální ostření v UI, CLI, dekodéru a SDK přijímá 0–200 %. Hodnota 100
má původní význam; 200 zdvojnásobuje zesílení i limit detailu v jednom
průchodu. Zachovává ochranu barev a alfa kanálu. Interní nativní wavelet
ostření je odlišný parametr a ponechává svůj rozsah 0–100.

CPU ostření nahrazuje kopii celého RGBA obrazu řádkovými buffery se
zmrazenými hranicemi pracovních pásem. AVX2 počítá jas původního řádku
jednou a znovu jej používá. Alokace probíhají před zápisem do obrazu.
Samostatný GPU průchod se dvěma AVX2 CPU workery se nyní provede na CPU,
protože v měření vychází levněji než upload a readback. Navazující ostření
obrazu, který je již na GPU, zůstává součástí GPU řetězce.

GPU kontury mají místo 16 vláken s float3 48 vlastníků barevných složek.
Pořadí rekurence a zaokrouhlení se zachovalo; počet dispatchů ani sdílená
paměť nevzrostly. Každou složku po oba směry vlastní jedno vlákno.

Párové měření proti R600.1, RTX 3070 / Ryzen 5600X, 2560×1440,
ostření 100 %. Samostatný filtr: čtyři obrácené páry, 64 vzorků na variantu.

- CPU 1 worker: 5,741 → 4,110 ms (−28,4 % času).
- CPU 2 workery: 3,417 → 2,133 ms (−37,6 %).
- CPU 10 workerů: 1,899 → 0,928 ms (−51,1 %).
- GPU volba / 2 workery: 3,138 → 2,125 ms; nový průchod používá CPU.

Celé video decode: 240 snímků ve dvou obrácených párech, ostření 100 %.
CPU 80k: 23,034 → 21,788 ms/snímek (−5,4 %), CPU 30k: 19,535 → 19,119
(−2,1 %). GPU se dvěma větvemi 80k: 12,522 → 12,465, 30k: 10,018 → 9,875
ms/snímek. Velké celkové GPU zrychlení se v tomto měření nepotvrdilo.
Profilované čekání na GPU contour/upscale kleslo přibližně o 9,7 %;
jde o celý úsek, nikoliv izolovaný čas samotného contour kernelu.

Ověření: finální Release 11/11 (18,87 s), ASAN 11/11 (222,84 s),
1 440 snímků přesné shody RGBA/PTS/duration proti R600.1, dalších 1 200
snímků přes SDK včetně seek/stride/guard kontrol. Nových 200 % ověřeno
na 160 snímcích a 20 seekách CPU/GPU; SDK odmítá 201. Tři UI průchody
ověřily video, přeskakování, fotografii, živé ovládání a uložení 200 %.
Kódová sekce DLL po konečném přeznačení má totožný SHA256 s měřeným
kandidátem. Důkazy: dev/diagnostics/FilterPerformance-20261030/.

# R600.1 — srozumitelný naměřený decode, 30. 10. 2026

Vydaná verze0.11.600.1 je v release/R600.1; hlavní spouštěč a dev/SDK
používají tuto verzi. Nastavení se převzalo z R600. Původní R600 a její
ověřený ZIP zůstávají zachované.

Hlavní ukazatel přehrávání zobrazuje „decode průměr … ms/snímek
(… snímků/s)“. Samostatně uvádí latenci konkrétní úlohy a čas převzetí
hotového obrazu z fronty. U přímé cesty se poslední údaj jmenuje příprava
snímku. První jednotlivý snímek/seek zobrazuje svou latenci, průměr se
objeví po nejméně4 dokončených snímcích. Nápověda vysvětluje význam čísel.

Průměr vychází ze sjednocení skutečně aktivních intervalů přípravy a decode
v obou větvích. Souběh se započítá jednou, žádné dělení počtem větví.
Čekání na plnou frontu nebo tempo UI se nezapočítává. Kruhové pole33bodů
udržuje posledních32 úspěšných dokončení bez alokace. Po přeskoku a novém
generation se měření resetuje; starý pracovník nesmí ovlivnit novou řadu.
Jde o propustnost dekodéru při práci, nikoliv zobrazené FPS videa.

Opraveny jsou i názvy nabídky enkodéru na R600.1 CPU/GPU. Profilové
identifikátory a formát zůstávají stejné. Kodek, komprese a obraz se touto
opravou nemění.

Ověření: Release11/11PASS18,30s; cílený ASAN playback PASS; dva nové UI
smokes včetně skutečného textu průběžného průměru, latence, přeskoku a
aktivace2 větví. Deterministické testy s řízenými časy ověřují26ms překryv
jako13ms/frame, posunutý překryv jako18ms/frame, vynechání pauz, posledních32
vzorků, chyby a reset. Integrační testy ověřují i pixely, cancellation,
reset při seeku a propagaci statistiky přehrávači.

ZIP má241 ověřených souborů; podklady jsou v
 dev/diagnostics/PlaybackTiming-20261030/release-validation.json.

---

# R600 — GPU čištění a menší CPU práce, 30. 10. 2026

Vydaná verze0.11.600.0 je v release/R600. Hlavní spouštěč a dev/SDK používají
R600. ZIP R600-20261030.zip má241 ověřených souborů a neobsahuje osobní
nastavení ani smoke výstupy. Nastavení se převzalo z R590.1. Její balíček
ani archivy R590/R590.1 se nezměnily.

Změny:

- Přesný GPU renderer základního čištění navazuje na kontury, zvětšení a
  ostření na zařízení, s jediným finálním readbackem mezi těmito filtry.
  CPU stále připravuje vodicí data a potřebuje nativní RGBA readback.
  Vodicí data se připravují a balí mimo zámek GPU enginu.
- Volitelný ticket nativního GPU obrazu používá omezenou cache se čtyřmi
  sloty. Při evikci se znovu uploaduje nezměněný platný obraz z RAM.
  Identita obsahuje unikátní stamp, adresu a velikost; COM pointer ani
  půjčený engine buffer nepřechází k volajícímu. Test vynutí hit i eviction.
- Renderer zachovává CPU zaokrouhlení reciproké hodnoty podle ověřené
  malé tabulky daného procesoru. Pokud se předpoklady nepotvrdí, funguje
  CPU cesta. Oracle zahrnuje72 konfigurací, malé/liché rozměry a alpha.
- CPU Max sdílí jednu kanonickou rekonstrukci pro referenci i samostatný
  zobrazovací oracle. U vnitřního čištění se zachovává správný vnitřní
  datový rozpočet, odlišný od běžného motion zobrazení.
- Připravený Native/Layout se sdílí mezi pokusy o přesnou alternativu Max.
  Max může posoudit nedávnou i původní referenci, vybírá skutečně menší
  paket a stále dodržuje depth16 a nejvýše5sekund změněného obrazu.
  Immutable State sdílí pakety i koeficientový root bez opakovaných kopií.
  Dekodér zahrnuje omezenou cache původního Native rootu do session budgetu.
- Prázdná koeficientová pásma už nespouštějí predikci a prostorový encode,
  protože jejich nulovou délku nelze zmenšit. Reuse koeficientů nevede
  k serializaci zbývajících entropických proudů; ty využijí vlastní budget.
- Build skript počítá SHA256 přes .NET, takže nezávisí na autoloadu
  Get-FileHash z odlišného PSModulePath děděného při spuštění z Pythonu.

Párové měření proti R590.1: RTX3070/Ryzen5600X,10workerů,240snímků,
dvě obrácená pořadí, ostření50. Hashování probíhá odděleně od timing.

- GPU80k, 1 větev/větve: 16.185279 → 14.876482 ms/hotový snímek (-8.09 % času). CPU práce -20.14 %, upload -71.24 %.
- GPU80k, 2 větev/větve: 14.445372 → 12.960330 ms/hotový snímek (-10.28 % času). CPU práce -23.63 %, upload -68.24 %.
- GPU30k, 1 větev/větve: 13.982283 → 13.140944 ms/hotový snímek (-6.02 % času). CPU práce -23.80 %, upload -77.78 %.
- GPU30k, 2 větev/větve: 11.114694 → 10.054595 ms/hotový snímek (-9.54 % času). CPU práce -24.24 %, upload -77.78 %.

Propustnost dvou větví není latence jednotlivé souběžné úlohy, seek ani
produkční UI FPS. Absolutní časy mezi běhy kolísají; jde o párové průměry.
Nativní readback pro CPU vodicí data zůstává; upload klesá díky ticketu.

Párové Max encode (80GPU/40CPU snímků ×dvě pořadí):

- gpu-80000: 11842.600 → 11296.900 ms (-4.61 % času), CPU práce -10.93 %.
- gpu-30000: 7812.885 → 7812.190 ms (-0.01 % času), CPU práce -3.80 %.
- cpu-80000: 9015.615 → 8454.625 ms (-6.22 % času), CPU práce -2.78 %.

Přesný koeficientový encode (80snímků ×dvě pořadí):

- 80000: 2325.386 → 2288.605 ms (-1.58 % času).
- 30000: 2316.771 → 2153.723 ms (-7.04 % času).

Velikosti a SHA všech těchto souborů zůstávají stejné. U Max30k není
měřitelný celkový encode speedup. Na této herní sadě není další úspora dat.

Ověření finální verze:

- Release11/11PASS18,79s; AddressSanitizer11/11PASS192,55s.
- Dva finální Max exporty240snímků80k/14 a30k/6 jsou bajtově totožné
  s R590.1 (14596844 /5254003B). Proto zachovávají kompletní již změřené
  source-relative RGB/chroma/luma/flicker metriky i kvalitu a časování.
- More quality, oba profily40snímků: rovněž stejné SHA a velikosti.
- Přesný režim: CPU/GPU kontrola všech80snímků obou profilů a PTS/duration.
- Přehrávací decode5konfigurací ×240snímků: stejné RGBA checksumy/PTS,
  CPU/GPU, obě velikosti a ostření100; serial předchozí verze vs2větve.
- SDK dalších5×240: stejné pixely, random seeks a buffer guardy včetně
  stride s28B padding. Dva UI smokes PASS včetně seeku a aktivace2větví.
  Skrytý timer smoke není důkazem produkčních FPS.
- Video integrační test: cancellation zachová cíl, cesta256MiB, sampling,
  korekce, koeficienty, Max serial/overlap a uzavřené reference PASS.

Zamítnuté pokusy jsou zdokumentované: sparse guide scatter byl pomalejší
než dense12floatů připravených mimo zámek. Vynucené root restarty a producer
capture v samostatném přesném režimu zpomalily herní encode asi9–10 % a
zvětšily soubor asi0,24 %. Tato kombinace není zapnutá; přesný režim používá
původní kratší recent řetězce. Klipový kvantizér a borrowing zůstávají OFF.

Zbývající náklady jsou CPU analýza vodicích dat, její nativní readback a
úplný nezávislý encode jako oracle kvality Max. Jejich odstranění vyžaduje
nové měření a ověření kvality. Vydání nepředstírá vyčerpání všech možností
optimalizace ani záruku bez artefaktů na libovolném ztrátovém nastavení.

Podklady: dev/diagnostics/ArchitectureNext-20261030/release-validation.json,
final-gates.json, full-packets.json, final-comparison.json, encode-comparison.json,
coefficient-pipeline.json, quality-mode-validation.json, SDK/pixel/UI protokoly.

---

# R590.1 — opravy po závěrečném auditu, 30. 10. 2026

Vydaná opravná verze0.11.590.1 je v release/R590.1; hlavní spouštěč a
dev/SDK používají novou verzi. R590 i její ZIP zůstávají zachovány.

Potvrzené a opravené chyby:

- Koeficientová cache kontrolovala payload a rodiče při lookup, ale při
  zápisu uložila i odlišný PacketView pod původní immutable identitou.
  Nový test s platnými odlišnými rodiči před opravou selhal (repro-coefficient,
  assertion80); po opravě zachovává přesné koeficienty původního snímku.
- Pohybová cache také ukládala výsledek pod neověřenou keyIdentity.
  Test používá dva platné motion pakety, cizí identitu a následný závislý
  snímek. Před opravou selhal (repro-motion-cache, assertion363), po opravě
  vrací původní přesné pixely. Podmínkou chyby byla upravená kopie ve
  veřejném C++ API; běžné neměnné pakety čtené z kontejneru se nemění.
- Max kontroloval prostorovou barevnou chybu, ale chyběl lokální jasový
  noise guard. Šedá šachovnice +/-4 prošla proti hladkému offsetu+4 se
  stejnou RGB MSE a nulovou chroma chybou (repro-quality, assertion111).
  Nový lokální luma gradient hlídá speckles a švy, včetně hran dlaždic.
  Má nezávislý pixelový oracle a stejné výsledky pro1/4workery.

První globální luma pravidlo bylo odmítnuto: zhoršilo celkové barevné a
časové metriky při častější intra náhradě. Finální lokální ochrana ponechává
ostatní RGB/chroma/flicker brzdění. Žádné tolerance nebyly plošně zvýšeny.

Ověření:

- Release11/11 a AddressSanitizer11/11 PASS; SDK zahrnuje1620kombinací.
- SDK proti vydané R590:5konfigurací ×240 snímků, přesné RGBA checksumy,
  PTS/duration, random seeks a guardy včetně odsazených řádků. CPU/GPU,
  30/80k a ostření100 navíc. Hashování je oddělené od timing.
- Dva nové240snímkové Max exporty, 80k/tolerance14 a30k/tolerance6,
  bez GPU fallbacku, deterministic random seeks. U30k mají stejné metriky
  a velikost. U80k soubor+9330B (+0,064 %), RGB MSE+0,016 %,
  chroma MSE+0,115 %, mean luma gradient RMS-0,020 %, mean stable flicker
  RMS-0,027 %. Není to plošné zlepšení všech metrik. Měří se vůči zdroji,
  průběhy v quality-comparison.json; RMS hodnoty jsou průměry po snímcích.
- Matched CLI80snímků ×2pořadí:80k11982,47 →12074,99ms (+0,77 %),
  30k8383,76 →8313,03ms (-0,84 %), deterministic SHA v každé verzi.
- SDK single-lane GPU80k16,6974 →16,5848ms;30k14,2602 →14,2764ms.
  CPU30k19,1817 →18,9485ms. První CPU80k běh ukázal+2,6 %;
  cílené4obrácené páry jej nepotvrdily (23,0951 →22,9512ms).
  Opravy se nevydávají za nové velké zrychlení. Nejde o UI FPS.
- Dva UI smoke průchody PASS včetně seeku a aktivace2decode větví.
  Timer smoke nedokládá produkční FPS přes CompositionTarget.Rendering.

Prověřené oblasti: kontejnerový index/CRC/atomic commit, meze entropie,
SDK stride a buffer guardy, reference a cache identity/lifetime, session
budget a pool, playback generation/cancel/shutdown, sdílení CPU workerů,
GPU locks/logical buffer size/readback/fallback a motion quality/repair.
Vydání obsahuje konkrétní ověřené opravy; není důkazem absence všech chyb.

Zbývají architektonické výkonové ceny: CPU/GPU roundtrips přes základní
čištění, úplný intra encode jako quality oracle a omezený výběr referencí.
Intra rate control může při nízkém rozpočtu stále časově kolísat. Ztrátový
Max ani silné ostření nejsou zárukou obrazu bez artefaktů na všech vstupech.
Globální luma experiment ani klipový kvantizér nejsou zapnuté výchozí.

Podklady: dev/diagnostics/FinalAudit-20261030/release-validation.json,
quality-comparison.json, sdk-pixels.json, sdk-timing*.json, encode-timing.json.

---

# R590 — rychlejší přesný decode, 30. 10. 2026

Vydaná R590 (0.11.590.0): release/R590, hlavní spouštěč a dev/SDK.
R580 zůstává zachována jako předchozí verze.

- Přesná AVX2 predikce koeficientů a obnova wavelet pásem. Stejné
  zaokrouhlení, omezení, float hodnoty a skalární záloha; bez nových ztrát.
- Dekodér používá již uložený prázdný skeleton a recykluje jedno volné
  referenční úložiště v rozpočtu session. Vrací ho až poslední vlastník;
  nová půjčka má nový control block, staré weak handles expirují.
  Vypůjčené spany zůstávají platné i po evikci cache. Pool je uzamčený
  a po zániku vlastníka nepřijímá další pole.
- Návrat k přesně stejnému zakódovanému keyframu po jiném snímku může
  použít 8 B odkaz. Pětisekundový limit dalšího změněného obrazu platí.
- Párové měření proti R580: 240 snímků × dvě obrácená pořadí, GPU,
  10 workerů, ostření50, bez hashování. Dvě větve: 80k16,004800 →
  13,270902 ms/hotový frame (-17,08 %); 30k12,702243 →10,631745
  (-16,30 %). CPU práce -30,79 % / -24,62 %. Serial80k20,342482 →
  16,331464 (-19,72 %), serial30k15,979653 →13,614775 (-14,80 %).
  Throughput NENÍ latence jednotlivé souběžné úlohy, seek ani UI FPS.
- Encode80 snímků ×2pořadí: 80k11782,10 →11727,55ms (-0,46 %),
  30k8130,565 →8014,710ms (-1,42 %). Všechny soubory pro daný budget
  mají přesný SHA shodný s R580; nová úspora na tomto klipu nenastala.
- Release11/11 a AddressSanitizer11/11 PASS. Pět konfigurací ×240
  snímků má přesné RGBA checksumy, PTS a duration proti R580.
  CPU/GPU30/80 a GPUsharp100. SDK testy zahrnují1620 přesných kombinací.
  Dva UI průchody včetně seeku PASS, dvě decode větve se aktivovaly.
  Hidden smoke používá timer; jeho presented počty nejsou produkční FPS.
- Výchozí AQ a ochrana textu zůstávají vypnuté. Fast/More quality a
  společná volba CPU/GPU jsou zachovány. Starší CLI aliasy zůstávají.

Podklady: dev/diagnostics/PostR580-20261029/{final-comparison.json,
image-validation.json,encode-comparison.json,release-validation.json}.
Plný resident GPU řetězec přes CPU základní filtr, odstranění úplného
intra oracle a širší reference zůstávají větší nedokončené projekty.

---

# R580 — paralelní přehrávání a cache, 29. 10. 2026

R580 (0.11.580.0) je vydaná: release/R580, ZIP R580-20261029.zip.
Hlavní spouštěč používá R580; dev/SDK odpovídá vydané DLL. Níže uvedené změny z 28. 10.
jsou jeho součástí; historické poznámky o ponechání R570 platily během
vývoje. Vydaná R570 zůstává v samostatné složce jako předchozí verze.

- PlaybackDecoder používá dvě nezávislé decode větve s oknem pěti snímků,
  chunk 4 a společným limitem CPU vláken. První snímek a seek jsou přímé.
  Automatika platí pro compressed GPU continuous view nad podporovaným
  nativním obrazem. RAM plán počítá dvě větve, zachovaný seek context,
  admitted jobs a původní UI ready queue; při nedostatku zdrojů je serial.
- Fronta vlastní frames, má pořadí, epoch cancel, content revision,
  bounded okno/cache, zpracování chyby a shutdown. Poslední snímek se
  zobrazí i při nedělitelném kroku. Není nový formát ani filtr obrazu.
- Párová měření 240 snímků x2, oba pořadí, GPU/10 vláken/ostření50:
  80k profil 18,713229 → 15,298324 ms/hotový snímek (-18,25 %),
  30k profil 15,545000 → 12,447990 (-19,92 %). Jde o throughput, ne seek
  či worker latency. Souběžná úloha má vyšší latenci; CPU práce stoupla
  o 31,5 % / 20,3 %. U 80k přibylo ~4,3 % uploadů a 2 % downloadů kvůli referencím.
  Referenční serial baseline je před recyklací bufferů, po změnách28.10.
- GPU buffery oddělují aktivní velikost od kapacity. Mírné headroom a
  logické kopie odstraňují alokační churn bez přenosu výplně. Jednotlivý
  buffer nejvýše256MiB; nezvykle velká kapacita se při zmenšení ořezává.
  U80frame encode:1741 ->357 alokací (-79,5 %), kumulativní alokované
  bajty31,25 ->2,91GB. To NENÍ současně rezidentní VRAM. Wall-0,19 %,
  CPU-2,46 %; nelze tvrdit výrazné celkové zrychlení encode touto změnou.
  Všechny čtyři zakódované soubory mají shodný SHA256.
- Tři decode větve ani dynamické půjčování CPU workerů nepomohly.
  Borrowing zůstává experiment default OFF. Nový prefix-clear shader
  byl po párovém testu odstraněn (o0,4–1,6 % pomalejší).
- Validace pixelů je oddělená od timing: plných240 snímků na CPU/GPU,
  30/80k a GPU sharp100 navíc, kontrola celého RGBA/PTS/duration.
  Závěrečná kontrola R580 je v DecodeAhead-20261029/image-validation.json.
- Běžný Release build:11/11 sad PASS, včetně GPU a SDK1620 přesných
  kombinací. ASAN rovněž 11/11 PASS (172,04 s). Čtyři UI průchody PASS;
  nové dvě větve se prokazatelně aktivovaly. Přímé srovnání smoke počtu
  zobrazených snímků neprokázalo zrychlení UI; nevydávat native gain za FPS.
  Nativní probe měří timing bez hashování. UI smoke používá časovač,
  takže jeho presented počet NENÍ produkční FPS (produkce používá Rendering).

Reprodukce: diagnostics/DecodeAhead-20261029/benchmark-release.py,
verify-images.py, validate-ui.ps1; release-throughput.json,
buffer-comparison.json, hardware-summary.json. Pro měření nespouštět
současně build/test/UI/hru ani druhý benchmark.

# Implementované opravy architektury (28. 10. 2026, development po R570)

Zveřejněná R570, ZIP, dev/SDK a hlavní spouštěč zůstávají vydanou verzí.
Změny níže jsou ve zdrojích a build-r4; QA publikace je pouze v diagnostics.

- DecodeSession vlastní hlavní cache pro každý C++/SDK decoder. Rozpočet
  256 MiB se ořezává mezi voláními, UI jej přizpůsobí explicitnímu RAM limitu.
  Testuje se střídání vlastníků, změna vlákna i nulový limit cache.
- GPU upscale → finální ostření má jediný readback. presentation-pair.json:
  240 snímků × 2, GPU/12 vláken/ostření 50, mean 20,9849 → 19,1256 ms
  (-8,86 %), p95 37,6091 → 35,8303 ms, přesné pixely/PTS/guardy/fresh seeks.
  Baseline je místní ArchitectureFix-20261028/before.dll po první hold opravě.
- Max má lokální lossless nativní opravy v137/mode3 a transakční výběr
  přesné alternativy. P musí šetřit proti této alternativě a projít všemi
  quality gates. gpu-80000-max14-cost.fajkv má 14 587 514 B proti R570
  14 757 536 B (-1,15 %). RGB MSE +0,77 %, chroma MSE +0,60 %, stable
  flicker mírně nižší, motion flicker prakticky shodný; není to plošné
  zlepšení kvality ani 60% dodatečná úspora. Viz quality-80-cost-full-summary.json.
- Max příprava následujícího intra je omezená dvěma položkami, při limitu
  10 vláken používá 2 přípravná + 8 závislých. schedule-comparison.json:
  dva 80snímkové průchody pro každé pořadí, 11 779 → 11 244,25 ms
  (-4,54 %), všechny čtyři soubory bajtově stejné. Jde o throughput převodu,
  nikoli záruku stejné relativní úspory na každém klipu.
- PlaybackDecoder odděluje řízení přehrávání od XAML. Ověřeny generace,
  bounded prefetch, chyby, content revision, lifetime a shutdown.
- FramePool používá celkových 64 MiB a znovu použije i DCI 4K buffer.
- Opraven stale hold po seeku zpět na předka. Automatika fotek správně
  zachovává 4:4:4; ručně zvolených 4:2:0 se nedotýká.
- Progress sčítá GPU compute práci všech přípravných workerů a calleru.
  Nová regrese provede skutečný image encode v workeru a kontroluje GPU
  transfery i tehdy, když caller neprovedl žádnou compute práci.
- final-checks.log: všech 11 sad PASS, SDK zahrnuje 1 620 přesných decode
  kombinací s lokálními opravami, holdy, seeky a padded RGBA řádky.

Odmítnuté nové experimenty: lazy canonical + display zvýšily 80frame mean
11,551 → 12,137 s; samotný lazy display na 240 frame 34,205 → 34,960 s.
Pakety byly přesně stejné, ale tyto změny byly odstraněny. Plošná paralelní
entropie měla 34,054 → 34,182 s a vyšší CPU práci; v encode s již zachycenými
koeficienty se nepoužívá, ani u malých decode skeletonů. Samostatné zrychlení
celého videa této malé změny není potvrzené.

Klipová stabilizace kvantizéru je experiment default OFF. Původně nižší
flicker a velikost doprovázela téměř +8% chroma chyba. Podmínka zaplnění
rozpočtu Fast 97 % / Quality 99,5 % je připravená pro další ověření;
slabší kvalita se nesmí automaticky stát novým výchozím profilem.

Z auditu zůstává plný GPU surface řetězec přes kanonické/CPU základní
kontury, vynechání úplného intra oracle a širší výběr referencí. Tyto větší
změny nejsou vydávány za hotové. Starší záznamy níže jsou chronologická historie.

# Audit architektury a první hold (28. 10. 2026)

Úplné závěry jsou v `ARCHITECTURE-AUDIT-20261028.md`: hostitelské GPU
rozhraní a mezikopie, plný intra encode před Max pokusem, duplicitní
rekonstrukce, zamítání celého P při lokální závadě, časový rate control,
plánování závislostí a vlastnictví cache. Uložené klipy 80 kB / Max 14 a
30 kB / Max 6 mají pouze 38/240 a 28/240 pohybových snímků; koeficientové
fallbacky zachovávají nezávislý intra obraz i jeho případný měnící se šum.

Opravena zbytečná rekonstrukce prvního přesného holdu po právě zobrazeném
koeficientovém nebo pohybovém snímku. Index poskytuje levnou informaci o
bezprostředně následujícím holdu; jen takový předek si ukládá prezentaci.
Pohyblivé snímky bez holdu nedostávají další kopii. Formát na disku je
stejný, R570 ani hlavní spouštěč se nepřepisují. Regrese nejprve selhala,
potom prošla včetně skutečné GPU nativní cesty nad velikostním prahem.
160paketová diagnostická sekvence (80 původních + 80 holdů) je na CPU/GPU
pixelově shodná s předchozí DLL, včetně PTS, guardů a fresh seeků. Časy
tohoto průchodu nejsou kvůli chybějícímu záznamu zátěže a nezvykle vysokým
hodnotám považovány za release benchmark. Logy: `diagnostics/Audit-20261028/`.

# Výkon dekódování po R570 (27. 10. 2026)

Další pokusy po této změně: cache čtyř koeficientových referencí (limit
64 MiB na vlákno) zachovala pixely, ale na 80 kB měla průměr 21,04 →
21,09 ms a na 30 kB 17,51 → 18,48 ms. Byla odstraněna. Uchovávání
již sestaveného podkladu bylo také odstraněno: 80 kB 20,49 → 20,43 ms
neprokázalo přínos a naměřená CPU práce vzrostla. Experiment s opětovným
použitím chyby sousedního pixelu v kontrole kvality vytvořil stejných 80
paketů, ale dva 80snímkové exporty trvaly průměrně 12,51 → 12,91 s;
i ten byl odstraněn. Zachován je nový nezávislý test barevného gradientu
na lichých rozměrech a hranicích 32px dlaždic. Po návratu kódu prošlo
10/10 Release sad. Diagnostika a opakovatelný test exportu jsou v
`dev/diagnostics/PostR570-20261027/`.

Vývojová změna znovu používá již rekonstruované koeficienty, když na ně
následující pohybový snímek přímo odkazuje. Dříve se v tomto případě znovu
procházel celý koeficientový řetězec. Cache kontroluje identitu, obsah i
rodiče paketu; test ověřuje opětovné použití i odmítnutí poškozených dat.
Výstupní obraz a formát se nemění. Vydaná R570 ani její spouštěč se nemění.

Párový GPU test dvou 240snímkových exportů (2 průchody, zahřátí 16 snímků,
12 vláken) zachoval každý pixel, časování, guardy a testované čerstvé seeky.
80 kB / Max 14: průměr 22,69 → 20,19 ms, p95 59,41 → 40,84 ms;
30 kB / Max 6: 20,05 → 19,50 ms, p95 49,31 → 36,35 ms. První měření
80 kB proběhlo při výrazně horších a proměnlivých časech, proto je uložené
zvlášť a zde je uveden opakovaný průchod. CPU 30 kB (1 průchod) zachovalo
pixelovou shodu; průměr 23,05 → 21,75 ms, p95 54,58 → 43,20 ms.
Jde o jeden herní klip, ne záruku zrychlení pro každé video nebo seek.
Všech 10 Release sad prošlo. Logy a výsledky jsou v
`dev/diagnostics/PostR570-20261027/`. Tato změna nesnižuje velikost souboru
ani neřeší známé kolísání obrazu při velmi nízkém datovém limitu.

# Odšumění po R560.1 (27. 10. 2026)

Aktuální změny pro R570, srovnávací výřezy a zdrojové metriky jsou v
`dev/diagnostics/ArtifactGuard-20261027/`. Uživatel schválil čistší vzhled
výřezu z `clean-final/`. R570 (0.11.570.0) je vydané v `release/R570` a
`release/R570-20261027.zip`; hlavní spouštěč míří na R570. Archiv R560.1
zůstává zachovaný. Doklady finálního sestavení a balíčku jsou v
`dev/diagnostics/R570-20261027/`: 10/10 Release sad, UI fotka/video,
SDK CPU/GPU s guardy/PTS/čerstvými seeky, 21 přesných schválených GPU
ukázek a 16 přesných paketů vytvořených dodaným GPU CLI. ZIP má
241 ověřených souborů, 23 927 166 B a SHA256
`62d74ec19956135d72154ddfa5dc593a9edc6b6586025d6a6fd50f4089163e6b`.

Nové nízkobitratové snímky ukládají jemnější relativní barevný kvantizér.
4:2:0 barvy se navíc čistí samostatnou prostorovou rekurzí vedenou jasem;
výrazné jasové/barevné hrany omezují šíření. Síla je mírnější při vyšší
hustotě dat. Jemné směrové čištění jasu doplňuje odšumění bez dalšího
vyhlazování barevných detailů. Zkoušené silné čištění všech složek bylo
odmítnuto kvůli slévání barev. Nepřidává se míchání sousedních snímků.
Nulové filtrování vypíná i nové barevné čištění, mezilehlé síly se škálují.

Nativní GPU cesta nově podporuje i uložené barevné čištění. Inverze,
jasové vodítko, oba směry čištění a převod do RGB zůstávají na zařízení;
nepřibývá přenos floatových rovin na CPU. Každá linka má jednoho vlastníka,
který přenáší rekurzivní stav přes celý řádek/sloupec. Experiment se
sdílenou pamětí byl odmítnut: při stejných pixelech zpomalil reálné
přehrávání (80 kB/14: 23,99 -> 28,50 ms; 30 kB/6: 20,99 -> 21,25 ms,
dvě opakování, 240 snímků, 16 snímků zahřátí, GPU/12 vláken). Ponechává
se rychlejší původní organizace GPU rekurze.
CPU zůstává zálohou a referencí pro kontrolu GPU.

Max posuzuje skutečně zobrazený pohybový snímek proti aktuálnímu
samostatnému snímku a originálu. Kontroluje barevnou chybu, její prostorový
gradient, flicker ve zdrojově stabilních oblastech a lokální barevné bloky.
Povoluje nejvýše 3 % vyšší celkovou RGB MSE pouze při nezhoršení všech
tří měr barvy/šumu/flickeru proti alternativě se stejnou předchozí
prezentací. Lokální poměrové limity mají i absolutní prahy, aby drobná
odchylka v téměř čisté oblasti nezamítala celkově méně zašuměný snímek.
Testy nadále odmítají izolovaný barevný blok i barevné tečkování, přestože
by se ve statistice celého snímku schovaly za zlepšení okolí.

Odmítnutá predikce používá přesné koeficientové překódování samostatného
snímku. Zachycené kvantované koeficienty se znovu nedekódují entropicky;
smíšené koeficientové/pohybové reference se rekonstruují přímo z nich.
Krátké řetězce mohou znovu navázat na uložený klíč bez nadbytečného nového
keyframu; limit změněného obrazu pět sekund a hloubka nejvýše 16 platí dál.
Opraveny jsou i opakované snímky: cache miss/čerstvý seek používá stejnou
zobrazovací cestu jako skutečný předek, včetně jeho filtrů. Dříve mohl
pohybový repeat po klíči zobrazit jiný obraz než běžné přehrávání.

`quality-summary.json` porovnává 3×240 snímků proti předchozí vývojové
verzi (`DecodePerformance-20261027/final`), stejnému originálu a PTS.
Při 80 kB / síle 14 klesla RGB MSE o 13,47 %, barevná MSE o 14,79 %,
RMS barevného gradientu chyby o 2,91 % a pohybově zarovnaný flicker o
6,23 %. Statický flicker ale vzrostl o 9,10 %. Při 80 kB / 6 klesla
RGB/barevná MSE o 7,36/8,06 %, ale statický/motion flicker vzrostly o
12,33/5,05 %. Při 30 kB / 6 je RGB/barevná MSE vyšší o 0,60/3,26 %,
RMS barevného gradientu chyby nižší o 0,97 % a flicker přibližně o 11 %
vyšší. Časová stabilita při velmi nízkém limitu tedy zůstává otevřená;
nejde o univerzální odstranění všech ztrátových artefaktů. Flicker RMS
je vážen počtem skutečně stabilních vzorků, není to průměr RMS snímků.

Čistší agresivní obraz má také větší soubor: 14 757 536 místo 11 062 032 B
v tomto 240snímkovém srovnání. Prioritou této etapy je potlačení artefaktů.
Časy z quality probe nejsou benchmarky a nesmějí se použít pro tvrzení
o rychlosti. Další validační doklady shrnuje místní `CHECKPOINT.md`.

# Předchozí etapa výkonu po R560.1 (27. 10. 2026)

Rozpracovaná implementace a validační doklady:
`dev/diagnostics/DecodePerformance-20261027/CHECKPOINT.md`.
Vydaná R560.1, její ZIP, SDK a hlavní spouštěč se nepřepisují.

GPU dekodér má oddělené nativní, konturové a zvětšené buffery. Při celém
1113snímkovém koeficientovém klipu tím zmizela většina opakovaných alokací:
4517 -> 78, po přidání nativního řídkého uploadu 79. Nativní cesta nově
používá přesné index/float-bits uploady s hustou zálohou. Přenesené host
koeficienty se nemění; původní inverze a barvy jsou zachovány. Samostatné
párové testy ověřily všechny pixely, PTS, guardy a čerstvé seeky. Uploady
požadované aplikací klesly z 28,46 GB na 11,46 GB za klip; readback zůstává
27,20 GB. Není to fyzické měření PCIe ani současné VRAM. Stabilní role
držících bufferů záměrně ponechávají další pracovní paměť mezi snímky.

Max komprese má omezený krok rezidua podle hustoty dat (strop 64–128),
místo původních kroků přes 200. Jasové carry je omezené a přizpůsobené
rozpočtu. Nové reziduum 134/16 používá propojené sousední pixely v
referenčním filtru: původní krok dva nedokázal odstranit některé barevné
šachovnicové vzory. CPU a residentní GPU cesta mají shodné celočíselné
zaokrouhlení, stejných devět tapů a žádný další GPU transfer při decode.
Staré varianty /14 a /15 mají původní chování. Zlepšení vyžaduje nové
zakódování; starý paket neobsahuje vyhozenou obrazovou informaci.

Finální `final/quality-summary.json` porovnává 3×240 snímků stejného
Fortnite klipu proti originálu a starému Max. Při 80 kB / síle 14:
RGB MSE -32,97 %, chroma MSE -31,38 %, statický flicker -24,28 %;
úspora proti samostatným snímkům 41,52 % místo 59,00 %. Při 30 kB / 6:
RGB MSE -5,93 %, chroma MSE -4,27 %, úspora 22,55 % místo 26,32 %;
statický flicker +1,70 %, motion flicker mírně nižší. Při 80 kB / 6:
úspora 31,22 % místo 31,55 %, RGB MSE -0,60 %, ale chroma MSE +1,44 %
a motion flicker +3,36 %. Nejde tedy o univerzální zlepšení každého
obrazu/metriky; hlavní přínos je ochrana agresivních a nízkorozpočtových
reziduí. Hodnoty nejsou subjektivní procenta kvality.

První pokus s univerzálním stropem 64 byl odmítnut: při 30 kB vytvářel
příliš mnoho keyframů a zhoršil flicker o 15 %. Závěrečné limitování
zachovává limit bajtů, pětisekundový refresh a krátké seek řetězce.

Závěrečný párový decode (3×1113, GPU/12): 17,9512 -> 17,5730 ms,
čas -2,11 %, stejné pixely; CPU práce přibližně stejná. Encode (2×240,
GPU/10, včetně I/O): 80 kB/6 10,276 -> 10,289 s; 30 kB/6 9,477 ->
10,041 s; 80 kB/14 9,782 -> 10,679 s. Kvalitnější agresivní obraz tedy
stojí i přibližně 9 % času encode. Prošlo 10/10 Release a 4/4 ASAN sad,
staré Max soubory mají přesné původní pixely na CPU/GPU a všechny nové
exporty ověřené guardy, PTS a čerstvé seeky. Vše je vázáno na hashe
testovaných exportů a binárek pomocí `DecodePerformance-20261027/results.json`.

# Navazující audit CPU/GPU (26. 10. 2026)

Plán dalšího decode výkonu: `DECODE_PERFORMANCE_PLAN.md`. Doklady a opakované
měření skutečného koeficientového klipu ze screenshotu jsou v
`dev/diagnostics/HardwareUtilization-20261026/`. Při vypnuté hře: 6/10/12
vláken přibližně 18,11/16,78/16,54 ms/snímek. Nové časové značky rozlišují
koeficienty, submit, wait/readback a I/O; čítače doložily 4517 GPU buffer
alokací, kumulovaně 54,5 GB, během 1113 snímků. Je to provozní churn,
nikoli současná VRAM. Současné `gpu.*` intervaly jsou stále CPU wall time.
GPU kernely mají vlastní budoucí timestamp úkol v plánu.

V této etapě byl doplněn profiler a GpuActivity čítače, algoritmus ani
vydaná R560.1 nebyly změněny. Prošlo 10/10 Release sad. Běh scalingu se
zapnutou hrou je oddělený, neúplný a vyřazený. Podezření na race při
obnově pásma bylo vyloučeno (aktivní wholeBands=true). Sériová nativní
metadata a globální GPU mutex se nepotvrdily jako hlavní brzdy.

# R560.1 (26. 10. 2026)

Původní balíček R560 zůstává zachovaný; nový je release/R560-P1, verze 0.11.560.1. Vývojový kód přidává AVX2 blokovou
pohybovou predikci pro CPU/fallback. GPU dekodér drží predikci, přesnou kanonickou inverzi, barvy i referenční
filtr na GPU, s jedním konečným readbackem. Přesná inverze opravuje float
reciprokou normalizaci a skalární konce řádků; ty používají přesně zadanou
double konstantu. Bez podpory potřebných operací navazuje CPU inverze
během GPU predikce. Enkodér drží svou kanonickou obnovu na CPU.
Řídké koeficienty se posílají jako přesné dvojice index/float bits; GPU
vynuluje pole a rozloží nenulové hodnoty před inverzí. AVX2 balení má
limit 1M položek (8 MiB); hustá data používají původní přímý přenos.
Pozice ani hodnoty koeficientů se nemění, včetně záporné nuly.
Callback nesmí používat GPU; jeho chyba se propaguje a nespouští opakování
inverze. Při GPU fallbacku se sleduje, zda už inverze proběhla.

Max encoder při změně rozlišení vytváří nový nezávislý keyframe, místo
převzorkování nového zdroje na starou referenci. Limit cache zahrnuje
kapacitu vektorů; neúspěšná alokace volitelné cache nezahodí hotový decode.

Profiler: resident_reconstruction zahrnuje GPU inverzi/barvy/filtr. CPU
inverse se v záložní cestě může překrývat s warp; na CPU je uvnitř synthesis_total.
Tyto intervaly ani decode.total se nesčítají. `compare_motion_decoders.py`
má volbu `--cpu-work` pro měření procesorové práce bez Python hashování.
Doklady: `dev/diagnostics/PostR560-20261026/`.

# R560 CPU / GPU (26. 10. 2026)

R560 zachovává formát a obraz R550. Zrychluje barevnou rekonstrukci rezidua
přes AVX2 s přesnou interpolací a zaokrouhlováním. Úplný exact skip vynechá
inverzi a filtr po validaci entropie. Nativní SDK výstup odstranil jednu
alokaci a kopii RGBA. Cache jediného původního keyframu porovnává celý
payload a má limit obrazu i dat 32 MiB na volající vlákno.

Nastavení nového balíčku se přebírá z R550, vlastní R560 se nepřepisuje.
Doklady testů a porovnání: `dev/diagnostics/R560-20261026/`.
Profiler `fajk_motion_decode_profile VIDEO OUTPUT.csv cpu|gpu [WORKERS]`
rozlišuje entropii, warp, inverzi, barvy, referenční filtr a dekódování rootu.
`decode.motion.synthesis_total` obsahuje inverse/color; `decode.total`
obsahuje všechny dekódovací etapy. Vnořené intervaly se nesčítají.

# R550 CPU / GPU (26. 10. 2026)

Aktuální balíček je `release/R550`, verze 0.11.550.0. Volitelný Max používá
reziduum 134/15: barevné chyby opravuje proti aktuálnímu originálu a omezenou
jasovou chybu přenáší jen z poloviny. Jemné koeficienty potlačuje plynulým
prahem, prostorová filtrace chroma respektuje jasové hrany. Přesné shodné
části se opakovaně nevyhlazují a při kroku pod 4 je filtr identita.

Síla 0–6 používá jemnější křivku; dodatečná agresivní úspora plynule nastupuje
na 7–12. Hodnoty 12–16 mohou citelně zhoršit detaily i stabilitu v pohybu.
CPU fallback vyžaduje robustní podporu společného barevného posunu a respektuje
vypnutí jeho odhadu. Počet vláken 0 znamená auto i uvnitř Max enkodéru.

Hrubší pokus nad příliš velkým reziduem neopakuje GPU hledání pohybu,
transformaci ani přenosy. Kanonická inverze zůstává na CPU. Celočíselný
referenční filtr nyní na větších obrazech běží na GPU s přesně stejnými pixely;
malé obrazy a fallback využijí CPU. Velikost souboru ani GPU volba samy nezaručují
zrychlení proti starší verzi či samostatným snímkům. C ABI je stále 1.
Ověření a konkrétní limity: `dev/diagnostics/AdaptiveMotion-20261025/`.

Následující části jsou historický stav jednotlivých vydání.

# R500 CPU / GPU (23. 10. 2026)

Aktuální verze je 0.11.500.1, balíček release/R500. CPU/GPU se v UI vybírá
společně pro enkodér a dekodér. AQ a ochrana textu jsou výchozí vypnuté.
Fast / More quality zůstávají samostatné. Historické popisy níže zachycují
stav při jejich vydání; aktuální chování popisuje tato sekce a kořenové README.

Oprava .1 používá SSE2 porovnání čtyř RGBA pixelů současně. Absolutní
rozdíl je celočíselný, tolerance alfy je nulová, úzké okraje zůstávají skalární.
Ověřuje se proti nezávislému skalárnímu orákulu včetně výplně řádků a
nezarovnaných adres. SDK recykluje souvislý scratch při padded stride,
s limitem 64 MiB na handle. Přibylo 648 nativních kontrol C ABI nad skutečnou
DLL, včetně ASAN. [Měření .1](../diagnostics/R500-P1-20261023/RESULTS.md).

Kontejner v2 používá přímou referenci na nezávislý keyframe. Paket 132
ukládá přesné zdrojové rozdíly bloků 64×64; bezeztrátový keyframe tak dává
bezeztrátový výsledek, ztrátový zachová bodovou mez chyby reference.
Volitelná tolerance 1–16 vyhodnocuje každý RGB kanál všech pixelů bloku
16×16 proti zdrojovému keyframu. Alfa musí být shodná. Přibližné přebírání
je omezené počtem uložených snímků (výchozí 10); žádná delta není referencí.
Paket 133/4 ukládá přesné číselné korekce vůči kanonicky dekódovanému
keyframu (deblocking 100, ostření 0). Nové soubory 133 vyžadují R500 dekodér.
Nezávislé pakety a C ABI 1 zůstávají kompatibilní. Ztrátové atlasové a
ztrátové reziduální prototypy 133/1–3 jsou odmítnuté a nepoužívají se.

Při rozsáhlé změně nebo příliš velké korekci se zakóduje celý původní obraz.
Fast před rekonstrukcí reference zkusí nejvýše max(32, 95 % budgetu / 128)
změněných bloků. Jde o heuristiku rychlosti, nikoli entropickou dolní mez;
More quality toto omezení nemá. Fast ukončí také zjevně drahý pokus podle
odhadu z prvních osmi bloků při více než dvojnásobném projektovaném budgetu.
Pohybová kompenzace není implementovaná. Nezávislé snímky jsou stále výchozí.
Rekonstrukce reference v tolerančním režimu sdílí rozpočet s jediným
enkodérem; původní a přesný režim nadále používají omezenou paralelní frontu.
SequenceReader a dekodér sdílejí identitu nezávislého paketu, takže se
stejný keyframe nemusí dekódovat znovu při prvním rozdílu. Cache má limit
64 MiB a klíč backend/vlákna/postfiltry. Studio pro všechny snímky včetně
temporal používá opakovaně zapůjčený výstupní buffer.

Finální ostření se provádí jednou po úplném složení a rekonstrukci obrazu.
AVX2 a GPU integer varianta jsou shodné. GPU se použije u dostatečně velkého
obrazu při 1–2 CPU vláknech; při větším rozpočtu je rychlejší AVX2 včetně
přenosů. Síla 0 průchod vynechá. Hlavní kontury zachovávají účinek i nad
80/150 kB; nulová intenzita je vypne. Celkem je vloženo 24 shaderů.

Ověření a omezení: [R500](../diagnostics/Temporal-20261023/RESULTS.md).

# R490 CPU / GPU (22. 10. 2026)

0.11.490.1 přidává `EncodeSpeed::MoreQuality/Fast`, nezávisle na backendu.
More quality je výchozí a zachovává referenční R490 pakety. UI ukládá
`encodeSpeed=0/1`; chybějící klíč znamená 0. CLI `--more-quality` / `--fast`
funguje pro encode a measure. Fast vypíná kontextově oceněnou volbu lokální
přesnosti, Gram volbu přesnosti, RDO koeficientů a synthesis pilot; AQ
používá existující levný estimator. Zachovává source priority i uživatelské
ovladače. Povoluje jeden dobrovolný korekční průchod a 97% zaplnění cíle,
ale povinné opravy překročení byte budgetu zůstávají. Decoder a C ABI 1
se nemění; větší zrychlení encode může znamenat nižší kvalitu detailů.
Validovaná kvalitní baseline 0.11.490.0 zůstává v checkpoint/ a původním ZIP.

Vývojová sestava 0.11.490.0 přidává GPU fit barevného modelu: double statistiky
buněk, detailové chroma odchylky a integraci bilineárních patchů. Čtyři pixely
se pro patch momenty vyhodnotí jednou; skalární rovnice mají nezávislé GPU
lanes. Model používá double redukci a CPU ověření buněk u diskrétních hranic.
Podpora double shaderů se ověřuje zvlášť; bez ní zůstává GPU barevný převod
s CPU modelem. Podporované neprůhledné 4:2:0 snímky bez syntézního pilotu
uchovají původní GPU barevné roviny a přímo na GPU provedou chroma predikci
a první tři úrovně dopředného waveletu; CPU float roviny se pro transformaci
znovu nenahrávají. Zbývající 1/64 plochy dokončí CPU kvůli přesnému
skalárnímu zaokrouhlení. CfL zachovává původní párové součty jasu.
Cache dvou barevných zdrojů má limit 96 MiB na zdroj, klíč jobu a adresu,
nedostupný zdroj bezpečně vybere CPU. Adaptivní RDO a entropie zůstávají CPU.
Hotový zmenšený obraz se uchová v omezené cache bez opětovného uploadu.
GPU finální hranové zvětšení s původními souřadnicemi se volí při 1–2 CPU
vláknech; při více vláknech je AVX2 rychlejší včetně přenosů. Zaokrouhlení
může izolovaně změnit pixel o 1 úroveň. Při vypnuté ochraně textu a výstupu do 4 Mpx se kontury
a upscale mohou spojit do jednoho GPU úseku s jediným readbackem. RDO sdílí
kontextové ceny kandidátů, rozepisuje přesný dvanáctičlenný stencil a AVX2
počítá osmipixelové kandidáty a Gram příspěvky při původním pořadí součtů.
Vložených je 23 shaderů. Výsledky a testy:
diagnostics/R490-20261022/RESULTS.md. Níže je zachovaný popis R480.

# R480 CPU / GPU (22. 10. 2026)

Předchozí aplikace a SDK byly R480, verze 0.11.480.0. Formát zůstává
kompatibilní s R470, C ABI je 1. Následující popis GPU cesty a paralelismu
platí i pro R480. Závěrečné výsledky: diagnostics/R480-20261022/RESULTS.md.

Volitelné výpočetní backendy čtou a zapisují stejnou současnou rodinu R470.
Enkodér a dekodér lze v aplikaci zvolit nezávisle. Výběry se ukládají do INI
jako encodeBackend/decodeBackend (0 CPU, 1 GPU). Nemění ID revize ani kontejner.
Nové shadery v gpu_shaders.hpp se kompilují při sestavení, nevycházejí ze
starého reconstruction441_gpu.cpp. Ten zůstává pouze ve výzkumném benchmarku.

GPU encoder přesouvá antialiasované zmenšování, převod do Oklab a přesné
celočíselné statistiky zdrojových bloků. Rozhodování AQ/RDO, hledání rozpočtu,
forward wavelet a entropické kódování zůstávají CPU: samostatný waveletový
přenos s float readbackem byl na RTX 3070 proti AVX2 pomalejší.

GPU decoder po CPU obnově koeficientů ponechá šest úrovní CDF 9/7, hrubé
rekurzivní filtry, nativní směrovou obnovu, CfL průměry, vedenou barvu a Oklab
konverzi na kartě. Vrací až hotové pracovní RGBA. Při rozpočtu 1–2 CPU
vláken následující rekurzivní RGB dočištění také používá GPU: oba směry
rekurze mají lokální sdílenou paměť, float mezivýsledky se nestahují.
Maska vynechaných dlaždic, průhledné pixely a okrajové dlaždice zachovávají
CPU chování. Při vyšším rozpočtu vláken vyšla AVX2 cesta rychleji včetně
přenosů, proto se vybírá CPU. Text guard, základní barevná rekonstrukce
a finální upscale používají současný CPU kód. Nejde o přímé
zobrazení GPU textury, takže jedno stažení RGBA zůstává zahrnuté v čase.
CPU encoderové transformace nejsou vydávané za GPU práci.

Zdrojové RDO váhy nyní rozdělují nezávislé řádky velkých mřížek mezi
pracovníky; původní úloha na celé pásmo nechávala jádra čekat na největší
jasovou mřížku. Každá buňka zachovává stejný výpočet a zaokrouhlení.

Malé obrazy, bezeztrátové/paletové formáty, alpha, diagnostický decode,
ruční doostření a nepodporované starší kombinace používají CPU. Samostatné
codec joby mají lokální scope; GPU fronta je chráněná mutexem, jiné decoder
handly mohou zároveň počítat entropii na CPU. Shodný neměnný zdroj uvnitř
jednoho jobu se znovu nenahrává. Recyklované obrazové buffery mezi snímky mají
jiný stamp, aby se nevrátil obsah předchozího videa. Paměťové limity a chyby
zařízení přejdou na CPU bez změny vstupních rovin. Čekání na GPU používá
událost místo aktivního čekání ovladače, pokud ji zařízení podporuje.
Omezená cache uchovává čtyři vstupní RGBA buffery do 32 MiB (nejvýše 128 MiB
celkem pro tuto sadu). Klíč tvoří job stamp, adresa a velikost; jednotlivé joby
si tedy při střídání původních a pracovních rozměrů nepřepisují zdroj.
Větší obrazy používají jeden buffer. Barevné převody ani shadery se nemění.

Float32 zachovává algoritmus, ne nutně každé zaokrouhlení. Diskrétní následné
filtry mohou zesílit izolovanou odchylku. Křížový decode a SSIMULACRA2 se
kontrolují zvlášť od přesné shody SDK/CLI na stejném backendu.
Video pipeline při alespoň šesti vláknech, dostatečné RAM a více snímcích
spouští dva nezávislé enkodéry pro CPU i GPU. Každý dostane polovinu celkového
rozpočtu; dokončené futures se zapisují v původním pořadí. Omezená fronta
čtyř úloh dovolí rychlejšímu enkodéru pokračovat, zatímco první snímek dobíhá.
Vnořené parallelFor dělí rozpočet mezi účastníky, včetně jejich vnitřní práce.
Dvě barevné inverse větve dostávají celkový limit, aby jejich vnitřní řádkové
transformace mohly využít po polovině vláken. Při menší RAM,
krátkém exportu a lossless zůstává jeden. Kontrolní součty kontejneru používají
osm kompatibilních tabulek IEEE CRC-32 místo závislého řetězce po bajtech.
Pakety, výchozí kvalita ani kompatibilita SDK se tím nemění.
Výsledky a reprodukce: diagnostics/R470-GPU-20261021/RESULTS.md.
Aktuální testy skutečného videa: diagnostics/R470-video-20261022/RESULTS.md.
Navazující optimalizace: neměnná tabulka RDO cen se připravuje jednou při
načtení modulu; horká cesta už nekontroluje inicializaci lokálního static
pro každý oceněný bit. Text guard znovu používá vodorovné intervaly pro
řádky se stejnou svislou podporou dlaždic. Okraje, pořadí a rozlišení
early/fallback se nemění. Obě úpravy zachovávají pakety i RGBA.
Měření, přesná regrese a vyřazené pokusy:
diagnostics/R470-dispatch-20261022/RESULTS.md.
Sekce níže s datem 18. 10. popisují předchozí CPU vydání a historii.

# Schválený výchozí profil R470 (18. 10. 2026)

Po schválení snímku 22885 je nová výchozí dodatečná úspora `flatSavings=0`.
Kontrast zůstává 50, práh 0, AQ zapnutá. UI jednou migruje tyto tři alokační
volby (`allocationDefaultsVersion=470`), další změny uživatele zachovává.
Skirt 50 / Tangents 0 / doostření 0 zůstávají; skryté volby AA a ochrany textu
se uživateli nepřepisují. Srovnání AVIF/JXL používá přesnou kopii aktuálního
INI, aby zahrnulo i tyto volby. Starší výsledky níže měřily úsporu 50.

# R470 — vydání po kontrole komprese a debug map

Aktuální vydání je `release/R470`; zdroje níže popisují současnou větev.
Základní místní AQ a ovladače navazují na sekci R465 níže.

R470 doplňuje detekci slabé tmavé kontury: nestačí nízký absolutní jasový
rozptyl k prohlášení oblasti za hladkou. Podmínka vyžaduje tmavý průměr,
silnou směrovou soudržnost a nelineární odchylku od RGB roviny relativně
gradientu. Skutečně plynulá tmavá rampa nadále zůstává plochou.

Opraveno přiřazení bitové mapy: dříve se celé náklady entropického bloku
rovnoměrně rozprostřely po velkém obdélníku. Nyní se stejné skutečné náklady
přiřazují pozicím ponechaných koeficientů s odhadem podle jejich magnitudy.
Počet bitů bloku je skutečný; rozdělení uvnitř bloku je pouze prostorový odhad.
Změna mapování sama nemění paket. Prázdné bloky se mapují rovnoměrně, náklady
mimo obraz se vykazují jako padding. Mapped + unmapped = celkové range proudy.
Předchozí ROI čísla ze staré mapy nelze přímo srovnávat s novým přiřazením.

Paketový inspector nově kromě součtu délek kontroluje i souvislé, nepřekrývající
se intervaly všech částí. Jednotkové kontroly hlídají zachování všech nákladů,
totožný paket s diagnostikou i bez ní a shodu SDK/CLI pixelů včetně guardů.
Žádná mapa alokace, zachycené koeficienty ani referenční encode nejsou uložené
jako další kopie v souboru. Přidaný capture a přiřazení běží pouze v Debugu.

Třetí ovladač `flatThreshold` (0–100 %, výchozí 0 %) snižuje potřebný hladký
podíl pro přepnutí do úsporné skupiny z 95 na 25 %. Ověřené hrany s pevným
okolím nepřeklápí. Je součástí obou cache klíčů, INI a automatického Debugu;
AQ vypnuto jej ignoruje. Referenční bilance při dodatečné úspoře/práhu
porovnává oba parametry na 0 při stejném kontrastu.

Výsledky a skutečné obrázky: `diagnostics/R470-final-20261018/RESULTS.md`.

# R465 — skutečná místní AQ a dvě volby alokace, 18. 10. 2026

Aktuální balíček je `release/R465`. Nízký tok s AQ používá nativní
koeficientový režim 32: dlaždice 8×8 koeficientů, skutečné místní kroky
0,25 / 0,5 / 1 / 2 / 4×. V hrubých pásmech je základní krok, aby adaptivní
volba neutrácela příliš mnoho za hrubé stínování. Výběr používá skutečné
pravděpodobnosti range coderu a cenu příznaků. Pilot zachytí skutečné vybrané
hodnoty v jednotkách základního kroku, včetně zlomků; nevyrábí zahazovaný proud.
RDO normalizuje cenu podle čtverce místního kroku. Enkodér je proti uniformní
AQ pomalejší; nejde o bezeztrátové zvýšení kvality všech oblastí.

Hlavní přepínač AQ je při startu vždy zapnutý. Vypnutí odstraní zdrojovou
klasifikaci, místní kroky, lokální ceny a dodatečnou úsporu. Globální krok,
pásmové váhy, konstantní RDO a vyhlazení zůstávají. Přepínač je součástí klíče
náhledu i exportu a testovací vypnutí se neukládá do INI.

Dva samostatné ovladače, oba výchozí 50 %:
- Kontrast a tolerance ploch (0–100 %). Při 100 % má nízký tok v místních
  RDO skupinách dvě ceny: 0,03125 pro geometrii a 128 pro zbytek. Tato silná
  krajní volba může odstranit skutečnou texturu. Při 50 % je přechod mírnější.
- Síla úspory v plochách (`flatSavings`, UI 0–100 %). 0 ponechá základní AQ,
  50 přidává až 8× vyšší cenu dat, 100 až 64×. Nepřestane fungovat při
  saturaci základní ceny na 128. Větší síla uvolňuje i vzdálené ochranné halo
  v prokazatelně hladkých oblastech: ponechává 8px okraj při 50 %, 4px při 100 %.
  Rozpoznaná hrana zůstává chráněná. DC se tímto krokem nemaže.

S dodatečnou úsporou je RDO grid jemnější: 4 / 2 / 1 koeficient podle pásma,
včetně hrubých predikovaných pásem režimu 32. Každý koeficient dostává vlastní
místní cenu; chráněná hrana nemusí zvýhodnit celý smíšený blok. Náklady predikce
jsou odhad, konečný limit kontroluje skutečná délka paketu. Grid se sdílí mezi
třemi směry a barevnými osami, není uložen do souboru.

Debug je opt-in. Nabízí samostatně skutečné bitové náklady, skutečné místní
kroky, způsobilost ploch a rozhodovací ceny alokace. Rozhodovací mapa může být
červená/zelená, zatímco mapa bitů zůstává plynulá: zobrazuje jiné veličiny.
Koeficient ovlivňuje více prostorových buněk. Zachycené ceny/bitové mapy se
neukládají do paketu a vypnutý Debug je nepořizuje.

Bilance při nenulové dodatečné úspoře srovnává stejný kontrast a sílu úspory
0 %. Při nulové síle srovnává kontrast 0 %. Vždy používá stejnou aktuální bílou
masku, umí jiné pracovní rozlišení i okrajové buňky. Bity v plochách i zbytku
jsou prostorové odhady detailových proudů bez DC a hlaviček. Druhý encode běží
jen při diagnostickém náhledu, po měření času kodeku. Záporná úspora se neskrývá.
Na každém 30kB obrazu nelze slíbit úsporu 2 kB ani obecné zrychlení.

Používat SDK R465: nový koeficientový režim 32 čte dodávaná DLL, C ABI zůstává 1.
Dekodér čte i předchozí režimy 23/24/31. Nepřibyl rekonstrukční filtr kvůli AQ.
Výsledky: `diagnostics/R465-final-20261018/RESULTS.md`.
Sekce R464/R462 níže jsou historické záznamy vývoje.

# R464 — korekce alokace a kvantizační chyby, 18. 10. 2026

Tehdejší vydání bylo `release/R464`; spouštěč a SDK musí být z tohoto vydání.
Sekce níže s označením R462 popisují starší stav, nikoli aktivní alokační režim.

- Alokační ovladač mění pouze source/RDO váhy. Nepřepíná mimo 50 % na
  koeficientový režim 24: jeho metadata a změna kvantizátoru s vypnutým
  pilotem část úspory rušily. Čtení starších režimů 24 zůstává.
- Úspora respektuje všechny rozpoznané hrany, i slabé; původní druhý silový
  práh část už rozpoznané geometrie z ochrany vyřazoval. Jeho duplicitní
  prefixová mapa byla odstraněna. Klasifikace a přímá ochrana sdílejí edge_.
- Tolerance určuje způsobilost smíšené plochy, nikoli úsporný násobitel celého
  footprintu. Při vyšší úspoře se cena škáluje skutečným hladkým podílem;
  dříve 25% hladký footprint dostal stejnou penalizaci jako 100% hladký.
  Na ověřených zcela hladkých plochách zůstává plná úspora zachována.
- Při nízké hustotě dat se provádějí dva opravné kroky podle skutečně
  vybraných koeficientů jasu. Druhý má gain 0,25, původní cíl zůstává
  neměnný, buffer pilotu se znovu používá pro chybu. Po každé opravě se
  invalidují meze dlaždic. Žádná nová dekódovací metadata ani filtr.
- Pilot má samostatnou instanci modelového trénování: zachovává adaptaci
  pravděpodobností, RDO a historii, nevyrábí zahazovaný range-coded proud.
  Volba je jednou na pásmo, nikoli podmínkou v produkční smyčce pro každý bit.
- Koeficientové RDO jednou normalizuje vstup a průběžně aktualizuje malou
  cache kvantizačních chyb. Neopakuje totéž pro každého souseda; pořadí
  výpočtů a rozhodnutí zůstává stejné. Izolované porovnání zachovalo celé
  soubory na devíti scénách/rozpočtech; samostatné zrychlení traineru se
  neprokázalo, zrychlení se měří společně s touto úpravou.
- Diagnostika ukládá opt-in náklady jednotlivých dlaždic před rozprostřením
  přes footprint. Plošná mapa není přesné vlastnictví bitů podle pixelů.
  Záznamy nevznikají při vypnutém Debug/budgetHeatmap.
- Přijaté rychlé rekonstrukční změny z předchozí vývojové iterace jsou také
  v R464. Výchozí ovladače zůstávají stejné, včetně úspory 50 %.
- Výsledky, omezení, zamítnuté pokusy a postup reprodukce:
  `diagnostics/R464-final-20261018/RESULTS.md`.

# R462 — jedno ovládání alokace a srozumitelný Debug, 18. 10. 2026

## Vývoj po auditu: rychlejší rekonstrukce a pilot kvantizace

Aktuální `dev/build-r4` obsahuje společně sestavené CLI, DLL a Studio.
Publikovaný release R462, kořenový spouštěč a distribuované SDK zatím zůstávají
na předchozím vydání. Nová bezeztrátová volba modelu koeficientů používá
selector 7 v režimu 31; budoucí vydání musí přibalit nový dekodér i SDK.

- Přijatých pět řezů kontury, předpočítaná geometrie, menší pracovní dlaždice,
  FMA s fallbackem a levnější tangent/chroma výpočet. Hustá síť u 30 kB zůstává.
  Produkce i Debug sdílejí výběr rozestupu kontur.
- Pilot nízké kvality používá skutečně vybrané celočíselné koeficienty jasu
  při odhadnutém kroku. Oprava invaliduje staré meze dlaždic. U vyšších hustot
  se drahá synthesis korekce vypíná; běžné ovladače/default 50 se nemění.
- Finální střídavé měření tří 2K scén: u 30 kB decode průměrně -6,49 %, ale
  encode +13,52 %; u 80 kB decode -11,41 % a encode -11,10 %; u 150 kB
  decode -2,07 % a encode -25,76 %. Jde o geometrické průměry poměrů časů.
- Vizuální posun je malý, některé ROI i skóre vyšších kvalit mírně regresují.
  Sub-16ms cíl není splněn univerzálně: 23006/30 kB má medián 16,33 ms a
  kontrolní snímek se psem 18,42 ms. Všechny filtry jsou v měření zahrnuté.
- 4/4 Release a 4/4 AddressSanitizer testů PASS. Reálné obrázky, konečné
  výsledky, kompromisy a zamítnuté varianty jsou popsány v
  `diagnostics/R462-edge-fit-20261018/RESULTS.md`.

## Opravy auditu R462, 18. 10. 2026

- Opraveno předávání SDK deblockingu/ostření přes scaled/flat/CfL obaly.
  Výchozí100/0 zachovává původní pixely. Samostatný AA/kontrast při nulovém
  Skirt se aplikuje vlastní existující rekonstrukcí, AA1/50/100 již není totéž.
- Automatický cíl má minimum1024B místo256B, které nepokrylo hlavičky.
  Jednobarevné RGBA snímky využijí existující přesný paletový formát bez
  transformace, modelu a filtrů. Žádný nový formát ani ztráta přesnosti.
- SourcePriority nestaví rozšířené mapy při50, sumy souřadnic počítá přesně
  jednou na buňku. Coupled quantizer jednou normalizuje zdroj pro všechny
  kandidáty. Resampler přeskočí nulovou korekci; vypnutá text protection
  recykluje neměnný snapshot. Část mrtvých historických větví odstraněna.
- Thread budget se kontroluje jednou na snímek a dědí do callbacků. Po
  vyčerpání práce se ruší pouze čekající prázdné callbacky, aktivní se spojí.
- Debug cache je výhradně diagnostická; při vypnutí se uvolní. Přepnutí
  pohledů neopakuje hotový produkční decode. Úpravy filtrů znovu používají
  koeficienty i skutečnou AQ mapu. Encode s mapami má výslovný diagnostický
  popisek, jeho čas se nemá považovat za běžnou kompresi.
- 4/4 Release i4/4 AddressSanitizer PASS;3872 decode případů, rozšířené
  mode24/420/422/444 mutace, malé rozměry, nezávislé ovladače.18 UI kontrol
  PASS; SDK9 paketů30/80/150kB×0/50/100 se shoduje s PNG, i s paddingem
  a guard bajty.12 skutečných neuniformních případů má stejné celé soubory
  i pixely jako původní R462. Default50 se nemění.
- Nové střídavé měření po spuštění zatěžující aplikace:3scény×3rozpočty,
  3kola×16opakování na verzi. Encode geometrický průměr poměrů−1.47%,
  decode−0.66%; malé rozdíly při proměnlivé celkové CPU zátěži56–80%.
  U150kB encode−3.82%, u30kB decode−2.68%, ale některé případy jsou pomalejší.
  Není prokázané plošné velké zrychlení ani stabilní decode<16ms.
- Uniformní2560×1440: celý soubor534→200B, encode32.34→0.84ms,
  decode10.11→4.63ms, přesné pixely. Nelze zobecnit na normální fotografie.
- Actual-rate quantizer a pilotní CfL korekce byly změřeny a vráceny:
  zpomalení nebo lokální kvalitativní regrese převážily nad malými zisky.
  Nové ROI/edge/SS2 limity v compare_encode.py zůstávají pro další práci.

Důkazy a přijaté/zamítnuté změny: diagnostics/R462-code-audit-20261018/RESULTS.md.

- UI má jeden flatAllocation posuvník 0–100, default 50. Staré čtyři ovladače
  z UI odstraněny; interní starší diagnostické volby zůstávají pro CLI/testy.
  Migrace z R461 zachová ostatní nastavení, nový ovladač začíná na 50.
- Při 50 se původní coverage i výsledné RDO váhy nemění. Pod 50 vyžaduje
  způsobilost k úspoře vyšší hladký podíl, nad 50 stačí menší hladký podíl.
  Pro vyšší toleranci SourcePriority ukládá i odstupňovaný kandidátní podíl
  slabé RGB textury podle rezidua od roviny a energie gradientu. Skutečné
  chráněné geometrické hrany a silná textura tento kandidátní podíl nemají.
  Prefix kandidátů vzniká během stejné analýzy; není potřeba další scan RGB.
- Kvantizační penalizace se upravuje i ve smíšené ploše zasahující halo hrany;
  dřívější flatSavings při original<1 celou úsporu přeskočil. Čistě chráněný
  footprint se nezhrubuje; při vyšší toleranci dostává nižší cenu přesnosti
  (při 100 % čtvrtinovou). Hrubý průměr/sklon nedostává úsporný
  násobitel. Mapa ploch zobrazuje účinnou způsobilost podle stejného nastavení,
  AQ mapy skutečné použité váhy. Jiná měřítka pásem mají jiné footprinty.
- Debug změnu alokace sloučí po 220 ms a provede nový diagnostický preview;
  běžná komprese zůstává ruční. Preview/cache/export zahrnují flatAllocation.
- Změněná alokace aktivuje skutečnou místní volbu kroků 0,25 / 0,5 / 1 / 2 / 4
  oproti základnímu kroku. Nejde jen o malou změnu RDO ceny při globálním kroku.
  Platí pro všechny kvality a cíle; default 50 ponechává přesný R461 profil.
  Pro vyšší úsporu zahrnuje klasifikaci až do 256px footprintu (jemná a střední
  pásma), nejhrubší pásma a DC ponechává bez úsporného násobitele. Skutečná
  silná geometrie uvnitř footprintu má před úsporou přednost; samotné halo
  nemá blokovat úsporu celé sousední stěny. Slabá koherentní textura není
  automaticky silná hrana. Při Debug změna kvality/rozpočtu/barvy i alokace
  po 220 ms obnoví diagnostický encode.
- Profil s místními kroky používá koeficientový režim 24. Nové pakety vytvořené
  mimo 50 vyžadují R462 (včetně nově přiložené SDK DLL); C ABI zůstává 1.
  Dekodér má dvě samostatně specializované větve pro tento režim, žádný další
  obrazový filtr/průchod. Výchozí režimy 23/31 zůstávají beze změny.
- Kolečko nad zavřeným výběrem pohledu přepíná položky; otevřený seznam normálně
  scrolluje. Nápověda ? obsahuje účel i skutečné ceny proudů. Ceny pohledů se
  nesmějí sčítat: koeficienty a rezidua zobrazují tutéž zakódovanou informaci,
  model C1/C2 má společnou kompresi. Mapy AQ a kontur se samostatně neukládají.
- Rekonstrukční filtry zůstávají stejné; nový koeficientový režim pouze přenáší
  odlišnou místní přesnost. Vyšší tolerance je záměrně ztrátová volba,
  není zárukou lepší kvality ani konkrétní úspory velikosti na každém snímku.

# R461 — Debug a barevné režimy, 18. 10. 2026

Balíček: `release/R461/app`, hlavní spouštěč už míří na R461. CMake a titulek
0.11.461, veřejný profil `r461`, interní ID 56 / rodina R441 zůstávají.
Uživatelská nastavení se jednou přenesou z R460, Debug se nikdy neukládá zapnutý.

- Samostatný `fajk_debug` pro aplikaci/CLI. Produkční `decodeWaveletImpl<false>`
  neobsahuje capture větve; `inspectWavelet` používá oddělenou šablonovou instanci
  a vlastní velké pracovní buffery. Běžný decode neprovádí diagnostické průchody.
- Debug má vlastní pravý panel, hlavní nastavení zůstává vlevo. Zapnutí režimu,
  změna dvojice pohledů nebo zesílení automaticky naplánuje novou analýzu.
  Krátké sloučení změn (60 ms) a generační token zabrání přepsání nového
  výběru starším výsledkem. Diagnostika má vlastní pracovní vlákno oddělené
  od komprese; jeho ukončení se při zavření aplikace synchronizuje. Vypnutý
  režim nespouští žádné diagnostické úlohy. Kolečko bez Ctrl zoomuje v místě
  kurzoru včetně přechodu z vystředěného obrazu do posouvatelného zvětšení.
- Debug má 21 pohledů, dvě volitelné strany a společný zoom/pan/export PNG.
  Y/C1/C2 jsou skutečné vnitřní složky před inverzním převodem, nikoli zpětný
  převod výsledného RGB. Rezidua jsou před CfL predikcí, koeficienty před
  inverzní transformací. Mapa kontur sdílí výsledné guide váhy s rendererem;
  není maskou celé finální rekurzivní filtrace. Rozpis offsetů účtuje každý
  bajt paketu jednou a rozlišuje kontejner od snímkového paketu.
- Na požádání se při enkódu zachytí skutečné finest-band RDO váhy Y/C1/C2,
  podíl hladké klasifikace a prostorově přiřazené entropy náklady. Mapy se
  neukládají do souboru. Barevná legenda má bpp a typické bity na 16×16 buňku;
  sonda počítá skutečnou plochu i v okrajové neúplné buňce. Váha není bitový
  příděl a RGB barvy nemají samostatnou přesně vyčíslitelnou bitovou cenu.
- Volby pod kvalitou: 4:2:0 (default), 4:4:4 a 4:2:2. Nový příznak 0x40
  značí pouze vodorovné podvzorkování; starší dekodéry jej odmítnou. 4:2:2 má
  vlastní horizontální konverzi a rekonstrukci bez vertikálního míchání.
  AQ a mapa ceny používají odlišný horizontální/svislý footprint. 4:4:4/4:2:2
  nepoužívají současný CfL model laděný pro 4:2:0; společné pracovní zmenšení
  při nízkém limitu platí dál. UI volbu ukládá, zneplatní preview cache a video
  pipeline ji nepřepíše automatickou volbou chroma. CLI encode/measure umí vše.
- 4/4 testy: core, reconstruction_r441, decode_robustness, debug_inspection.
  Nové testy ověřují skutečné rozměry barev včetně lichých hran, alfa,
  shodu vláken, součet bajtů, neměnnost paketu/pixelů a následného enkódu,
  odezvu AQ vah na alokaci a úsporu v hladkých plochách. <16pixelové rohové
  fragmenty zůstávají konzervativně neklasifikované, jako dříve.
- UI smoke: vypnutý start, přepínač chroma/cache, načtení diagnostik, zoom,
  návrat a uvolnění obrazu, neměnnost preview a generování skutečných AQ map PASS.
  Důkazy a obrázky: `diagnostics/R461-20261018`.
- Výchozí 4:2:0 22920 při 30/80/150 kB: pakety i RGBA přesně shodné s přijatým
  R460. Dvě střídavá kola po 8 opakováních: encode 44,07→45,04 / 67,11→67,43 /
  110,97→109,12 ms; decode 18,42→18,64 / 16,87→17,20 / 16,63→16,06 ms.
  Běh na zatíženém PC kolísá, p95 19–22 ms; <16 ms není všeobecně dosaženo.
  Toto vydání nepředstírá nové plošné zlepšení kompresního poměru.

# Aktuální architektura R460 — 17. 10. 2026

## Nejnovější přijatá iterace: společný barevný fit a kontury střední kvality

Aktuální zdroje odpovídají `diagnostics/R460-wide-20261017/tuned-cli.exe`.
GUI release byl 17. 10. 2026 sestaven do `release/R460/app` spolu s CLI
a aktualizovaným SDK. Prošly všechny 3 testovací sady a spuštění zabaleného
CLI. Log: `diagnostics/R460-continuous-20261017/build-release.log`.
Poslední pokusy o interpolované tečny, trojúhelníkový resampler a sdílenou
mapu jasu nepřinesly dostatečný přínos a byly před buildem vráceny.
Podrobnosti přijaté sady, zamítnuté varianty a měření jsou
v `diagnostics/R460-wide-20261017/README.md`.

- `color_prediction_fit.hpp` společně dolaďuje interpolované 64px barevné modely
  podle detailu v blocích 2×2. Dvě omezené iterace používají jednou nasbírané
  statistiky; sousední modely se nefitují izolovaně. Formát a operace dekodéru
  se nemění. Nejde zatím o fit proti skutečně kvantovanému jasu.
- Optimalizovaný výpočet dává na třech hlavních 30kB scénách shodné pakety
  jako uživatelem schválený `color-joint`. Přímý nezávislý gradientový test
  hlídá rohy, liché rozměry, konstantní jas a shodu různých počtů vláken.
- Při ekvivalentních 80 kB má hlavní filtr asi 15% účinek místo 11 % a
  rekonstrukce užší omezený přechod. Žádný nový průchod. Křivka kontur dál
  monotonně slábne; 30kB endpoint zůstává stejný, 150kB čištění zůstává vypnuté.
  Odhad síly v UI sdílí tuto křivku; ovladače ani jejich defaulty se nemění.
- `final-paired`: 22920 SS2 30 kB 6,562 → 7,472; 80 kB 42,192 → 44,003.
  Další dvě 80kB scény se zlepšily o 1,201 a 0,569 bodu. Herní 23006 / 30 kB
  ztrácí 0,191 bodu SS2, přestože RGB a hraniční MAE mírně klesají.
- Celý CPU decode: hlavní 30 kB 18,03 ms, 80 kB 17,16 ms; ostatní hlavní
  80kB scény 16,77/16,89 ms. Cíl stabilně pod 16 ms ještě splněn není.
  Všechny tři 150kB výstupy jsou pixelově i souborově shodné s výchozím stavem.
- Prošly 3/3 sady (`build-tuned.log`). Není přijatá drahá nová 150kB AA větev,
  zesílená místní AQ, širší barevný fit ani nové údaje o konturách v souboru.

## Předchozí přijatá sada (zůstává základem)

Aktuální pracovní zdroje: přijatá sada `diagnostics/R460-next`, ověření
`ready-check-20261017`. Release balíček se v této iteraci nevyráběl.

- Na nízké hustotě dat odpovídající 30 kB / 2560 × 1440 se pracovní rozměr
  mění z 13/16 na 12/16. Mezi ekvivalentními 30–40 kB plynule vrací 13/16;
  rozhoduje hustota dat, nikoli absolutní velikost. Při stejné velikosti
  souboru tak zůstane více přesnosti na pixel a méně kvantizačních skvrn.
  Zmenšení na 11/16 a plošné 12/16 včetně 80 kB byly zamítnuty.
- Text má řídké reference před základní konturovou rekonstrukcí. Silné
  skupiny tahů uchovají původní dekódované pixely; nejisté útvary používají
  referenci aktualizovanou po konturách. Intervaly jsou disjunktní, každý
  pixel se ukládá jednou. Nulové/slabé kontury neprovádějí drahý časný scan.
  Neutralita okolí se ověřuje před nákladným rozpoznáváním tahů. To brání
  dalšímu poškození písma filtrem, neopravuje již ztracené znaky.
- Enkodér při alternativním entropy kódování znovu používá již vybrané
  kvantované koeficienty; nedekóduje právě vytvořený proud. Koeficienty se
  zachytí po RDO a před reverzibilní predikcí. Samostatné testy ověřují
  shodu s původní cestou. SIMD resampler omezuje nepotřebné načítání a
  konverze se stejnými pixely. Rekurzivní cleanup používá dlaždice 128.
- `inspect-contours` sdílí výsledná vodítka s rendererem; CSV color/geometry/
  thin už není jen mezivýsledkem před regularizací. broad/fine zůstávají
  diagnostikou místních fitů.
- Sekvenční střídavé měření `final-main-20261017`: tři hlavní scény ×
  30/80/150 kB, dvě kola po 16 opakováních. SS2 u 30 kB: 22920
  5,655 → 6,562; 22925 9,733 → 11,198; 23006 20,602 → 21,401.
  U všech šesti 80/150kB variant jsou soubory i pixely přesně stejné jako
  před touto sadou. Hraniční MAE hry při 30 kB mírně stoupla: celkové SS2
  zlepšení se nesmí vydávat za opravu každé hrany.
- 22920 / 30 kB: encode 53,04 → 42,80 ms, celý CPU decode s připraveným
  výstupním bufferem 21,47 → 18,70 ms, p95 22,67 → 20,27 ms. Export PNG,
  start procesu a vykreslení UI nejsou součástí těchto časů. Hra na pozadí
  může měření ovlivnit. Stabilní <16 ms ještě není splněno a některé p95
  jsou nad 20 ms. Zrychlení není jednotné na všech snímcích.
- Pět dalších 30kB scén v `final-holdout-20261017` má SS2 změnu −0,001 až
  +0,218 bodu. Pes se srstí zůstává při 30 kB velmi nekvalitní. Text a obličej
  na hlavní scéně stále ukazují výrazné ztráty; všeobecné odstranění artefaktů
  ani kvalita srovnatelná s originálem nejsou dosaženy.
- Poslední ověření `ready-check-20261017` potvrzuje pixelovou i souborovou
  shodu aktuálních zdrojů s vybranými výsledky všech devíti hlavních případů.
  `build-ready-20261017.log`: core, reconstruction_r441, decode_robustness 3/3.
- Jemnější AQ pro nalezené tahy, zesílené AQ obecně, změny váhy barevné
  predikce, jasově tolerantnější plošné čištění, Lanczos a agresivnější
  downscale byly vráceny. Některé zlepšily text, ale poškodily zbytek scény.
  V produkčních zdrojích není glyph experiment ani nová konturová metadata.
- Ovladače a formát se nemění: Skirt 50 %, Tangents 0 %, plochy 0 %,
  kontury 200 %, AA 50 %, kontrast 0 %, ochrana textu zapnutá.

## Starší stav konturové iterace (nahrazen výše)

Pracovní kandidát: `diagnostics/R460-contour-flow`. Není zatím přijatý jako
plošné zlepšení kvality ani jako release. V konturové větvi váží barvy i
geometrii stejnou důvěrou, hledá přechod přes RGB projekci a u silné korekce
lokálně fituje křivku a sjednocuje kompatibilní sousední vodítka. Slabá
korekce zachovává levnější lokalizaci. Skirt, resampler a enkodér se nemění.

- Analytický test zvlněné kontury: součet polohové chyby 89,57 → 55,84;
  přičtení stejného jasu ke všem RGB kanálům výsledek nezměnilo. To není
  důkaz stejného zisku na skutečné komprimované scéně.
- Na 22920 / 30 kB má optimalizovaný kandidát SS2 5,655 proti 5,718 před
  změnou; krátké čtyřopakování celého decode bylo 19,22–21,07 ms. Starší
  neoptimalizovaný kandidát má střídavé měření v `paired/results.json`.
  Cíl <16 ms ani jednoznačné vizuální zlepšení všech scén nejsou splněny.
- Plošná difuze, agresivnější korekce a poslední zesílení barevného čištění
  byly vráceny. Jejich zdroje/výstupy zůstávají jen v diagnostice. Aktivní
  `edge_model_avx2.hpp` odpovídá `edge-optimized.hpp`, nikoli `edge-noise.hpp`.
- `-effect.png` používá skutečná výsledná vodítka rendereru. CSV inspectoru
  zatím popisuje mezivýsledky lokální analýzy, nikoli pole po sousední
  regularizaci; nesmí se vydávat za přesnou konečnou masku.
- Další konkrétní slabina: snapshot ochrany textu ve `fastReconstruct441`
  vzniká až po základní konturové rekonstrukci. Chrání před dalším čištěním,
  nevrací případné poškození textu z předchozího průchodu.

Nejnovější iterace pouze kontur: `diagnostics/R460-contour-debug`.
Enkodér, Skirt/Tangents, resampler a uložené ovladače se zde nemění.

- Při selhání původní geometrie (<0,03) se ověří také jemný barevný model,
  i pokud měl nepřesnou předběžnou přímku. Krátká rohová sonda zachytí detaily
  mezi řídkými vzorky. Nová větev má 0,75násobnou důvěru a nepřefitovává
  původní použitelné modely. Ty se mohou změnit až při společné interpolaci.
- Směrová shoda používá délku průměrné normály na druhou. Původní tvrdý práh
  0,5 rušil i směs kolmých normál na skutečném oblém obrysu. Opačné normály
  mají stále nulovou váhu. Stejný vzorec sdílí kontury a side-aware větev.
- `inspect-contours INPUT PREFIX [GRID=auto] [baseline]` ukládá nativní obraz,
  CSV důvěry/důvodů odmítnutí a `-effect.png` přímo z rendereru. Zelená je váha
  korekce tvaru, červená váha barevného lemu konturového modelu. Není to
  rozdíl výsledných pixelů ani celkový Skirt filtr. Baseline existuje jen
  v diagnostice a reprodukuje původní modely i původní směrovou váhu.
- Na totožných nativních pixelech 22920 vzrostlo pokrytí vahou >=8/255
  z 234 632 na 268 111 pixelů (+14,3 %), v měřeném úseku vlasů 493 → 617
  (+25,2 %). Herní 23006 má 205 540 → 230 012 pixelů (+11,9 %). Nejde
  o procenta vizuální kvality; mezery a ztracené detaily zůstávají.
- Test prudce zakřiveného obrysu vyžaduje >5% zlepšení skutečného pokrytí
  blízko analytické kontury: dosaženo 469 → 599 bodů. Ověřuje také černou
  masku při nulové síle a nulovou váhu u průhledného pixelu. Prošly 3/3 sady.
- Střídavé měření stejných paketů (`paired-speed`): hlavní 30/80kB snímky
  mají median celého decode 17,24–19,00 ms, typicky +0,1–0,35 ms. SS2 rozdíl
  šesti hlavních variant −0,082 až +0,010; UI text přibližně stejný. Další
  auto 30 kB −0,31 bodu, pes 30 kB mírně stoupl. Vyšší pokrytí není záruka
  vyšší věrnosti na každé scéně, stabilní <16 ms splněný není.
- Normální decode má kompilované specializace bez debug sběru, CSV a exportu
  masek. Report zahrnuje i poslední vodítka za okrajem lichých rozměrů.
  Explicitní skipBaseCleanup funguje také pro nativní obraz, takže inspector
  neanalyzuje už upravené pixely. Bez tohoto debug flagu se cesta nemění.

`debug-head` / `debug-whole` obsahují přesné masky a souhrn JSON,
`actual-comparison.png` skutečný výsledek, `high-density` nulovou váhu na
150 kB. Vývojové CLI je přeložené, release balíček nevznikl. Předchozí
agresivní `localized` kandidát byl vrácen, protože měnil i fungující kontury.

Poslední iterace: `diagnostics/R460-tolerance` (vývojové CLI, bez release balíčku).

- Kontury mají širší pokrytí celého obrazu. Při rekonstrukci s tolerancí zvonění
  klesl minimální kanálový rozsah 24 → 16, energie barevného přechodu 400 → 196
  a odchylka od roviny 3 → 1,5. Síla nadále roste plynule se spolehlivostí:
  slabší detekce nedostává stejný zásah jako čistý výrazný obrys.
- Barevné modely mají měkký konec tolerance místo úplného odmítnutí při mírném
  překročení chyby. Tolerance RGB rezidua roste u chromatických hran; neutrální
  tenké tahy mají původní silnou ochranu. Detailová váha chromatických kontur
  přechází plynule, i pokud sousední model zachytí několik průchodů hranou.
- Jemný model se ověřuje skutečným umístěním přechodů dřív, než nahradí širší
  model. U barevné kontury lze použít i dva vzdálené průchody s omezenou vahou;
  rozptyl průběhu má vyšší toleranci. Posun barevné fáze zůstává omezený.
- Odmítnuta hustší 6px síť: nevyřešila vlasy a zpomalovala render i analýzu.
  Používá se 8px síť, u slabé korekce 16px. V cestě s nulovými plochami se
  neinterpoluje nepotřebná část rovinného modelu; extrema vzorků se redukují
  přímo v SIMD registrech. Kontrola na šesti uložených výstupech potvrdila
  přesnou shodu pixelů před/po samotné optimalizaci interpolace.
- Vyřazena nízkotoká chroma synthesis kompenzace: dva pomocné chroma piloty,
  jejich zpětné/adjungované transformace a opravný průchod. Jasová kompenzace
  zůstává. Střídavé měření čistého encode (2 kola × 4 opakování/snímek) dalo
  55,56 → 50,42 ms, 60,10 → 53,68 ms, 57,50 → 55,47 ms. SS2 změny při stejné
  velikosti: +0,054, −0,078, +0,034; nejde o čistě bezeztrátovou optimalizaci.
  Z interního decode rozhraní byly odstraněny nepoužívané exporty chroma
  koeficientů a prostorových diagnostických rovin. Čtení starých paketů trvá.
- Nový test požaduje >1% snížení chyby slabé zakřivené barevné kontury a shodu
  1/3 vláken. Dřívější 24úrovňový práh tento 22úrovňový obrys zcela přeskočil.
  Průběžně prošly core, reconstruction_r441 i decode_robustness.
- Přímá diagnostika rozlišuje barevnou a geometrickou důvěru i ochranu tenkých
  tahů: `fajk_cli inspect-contours INPUT PREFIX 8`. Mapa rozdílu dvou výsledných
  8bitových obrázků není mapa detekovaných hran ani segmentace vlasů! Nástroj
  `tools/review_contour_guides.py` zobrazuje skutečné interpolované důvěry.
- Na totožném uloženém paketu 22920 přibylo proti předchozímu pokusu při stejné
  síti 17% použitelných modelů (8 571 → 10 045; práh důvěry >0,05). Vybraný úsek vlasů
  se změnil z 13/36 na 19/36 modelů. To neznamená obnovu všech vlasů ani 17%
  vizuální zlepšení. Obraz 30 kB stále viditelně ztrácí detail a má artefakty.
- Párové měření stejných paketů vůči hustší variantě (`paired-speed`) dalo
  u 30 kB median 18,75 / 19,88 / 18,59 ms, přibližně 8–10% zrychlení.
  80 kB nemá konzistentní zrychlení a p95 některých měření přesahuje 20 ms.
  Vlastní alokace výstupu přidává několik ms. Cíl celý decode <16 ms splněný není.

`refine-final` obsahuje poslední encode 30/80 kB; `final-review` je předchozí
výběr jemných modelů. Platný lokalizovaný jemný model se zachová i se slabší
důvěrou, širší model ho nahrazuje při selhání lokalizace. `chroma-ablation`
obsahuje oddělené měření odstraněné kompenzace. `holdout` měří také auto a psa:
150 kB zachovává přesné pixely, nižší kvality mají smíšené výsledky (například
auto 30 kB −0,60 SS2, pes 30 kB −0,21). Vyšší pokrytí kontur není důkazem
univerzálně vyšší věrnosti. Poslední krátké měření hlavních snímků vyšlo
16,5–18,0 ms pro 30 kB, ale dřívější běh za jiné zátěže dosáhl i 26,6 ms.
Nelze garantovat <20 ms. Kontrola vlastního výstupu / bufferu / jednoho vlákna
je v `output-check`; vlastní alokace přidává několik ms.
`chromatic`, `subpixel`, `band`, `grid6` a jiné
varianty jsou experimenty, nikoli aktivní profil. Barevné přepartitionování
modelu a nové směrové vzorkování nepřinesly přesvědčivý přínos a byly vráceny.

Aktuální profil je R460, interní ID 56 a rodina paketů R441 se nemění.
Předchozí popis R450 níže je historie, nikoli aktuální výchozí nastavení.

- Schválené defaults: Skirt 50, Tangents 0, plochy 0, kontury 200, AA 50,
  doostření 0, text protection zapnutá. UI migrace cleanupDefaultsVersion=2
  jednorázově mění čtyři uživatelem výslovně vybrané hodnoty; další úpravy zachová.
- decodeScaled i decodeBase sdílejí artifactReconstruction::restore. Dřívější
  duplicitní vzorec ve scaled větvi obcházel změny útlumu. Konturové modely
  slábnou od 40 do 150 kB/1440p, zanedbatelné korekce se přeskočí.
- Slabý Skirt (amount<=0.2, bez Stronger Tangents, curvature<=0.06) používá
  čtyři sousedy s jednou RGB vahou a chráněnou alfou. Jde o levnější aproximaci,
  nikoli numericky totožný rekurzivní filtr. Silná nízkotoká rekonstrukce zůstává.
- Zvětšování drží kontrast přechodu i při útlumu čištění; ruční Doostření hran
  používá existující uložené pole contrast a není vypnuto kvalitou. Ve scaled
  větvi je součástí interpolace; v nativní větvi boundedContourContrast po AA.
- Nativní dering nových high-rate paketů má sílu 24 místo 64. Úplné vypnutí
  zhoršovalo zvonění; ponechán omezený zásah. Chroma kvantizace 144 místo 160
  pouze mimo lowRate. Globální změna byla odmítnuta kvůli 30kB regresím.
- Výpočet kontur má specializaci pro nulové plochy. Nativní dering nekopíruje
  vnitřní vzorky, které následně všechny přepíše. Okraje/padding zachová.
- review_r460.py používá explicitní schválené filtry a porovnává skutečné PNG
  při stejném limitu. Encode_process_ms zahrnuje start procesu a IO, není to
  čistý čas kodeku. Decode median/p95 zahrnují všechny filtry do RGBA bufferu.
- Původní binárka a úvodní data porovnání zůstávají v diagnostics/R460/baseline*.
  Výsledky další iterace jsou popsané níže; bezartefaktový obraz zatím nemáme.

Další vývoj R460, 16. 10. 2026 (zatím bez finálního release balíčku):

- Při nízkém toku se posílila priorita středních waveletových pásem z 0,0625
  na 0,125. Jemné 4:2:0 barevné rozdíly se nově podílejí na fitu prediktoru:
  poloviční směs detailového a širokého modelu, pouze v 64px buňkách.
  Výpočet používá již převedené barvy uvnitř stejné buňky; nevzniká další
  celoplošná konverze RGB. Vysoká kvalita s 32px modelem zůstává beze změny.
- Nový vnitřní entropický model **31** vybírá pro každé pásmo mezi původní
  reprezentací (prefix 4 / model 23) a krátkými magnitudami (prefix 6 / model 25).
  Rozhoduje skutečný počet bajtů po zakódování. Převádějí se již kvantovaná
  celá čísla, proto tato volba sama nemění pixely. Aktivní jen v závěrečném
  nízkotokém uniformním kódování; adaptivní přesnost se takto nepřekóduje.
  Kontrolní tři 30kB snímky ušetřily 122–293 B koeficientových dat beze změny
  koeficientů. Není to úspora 30 → 20 kB. Rate search může prostor použít
  na přesnější koeficienty, proto celý nový encode může mít jiné pixely.
- DŮLEŽITÁ KOMPATIBILITA: rodina/ID se nemění, ale model 31 je rozšíření
  bitstreamu. Aktuální dekodér čte modely 23 i 31; dřívější R450/R460 binárky
  nové pakety s modelem 31 odmítnou. S novým enkodérem se musí dodat nový
  dekodér. Staré soubory není potřeba převádět.
- Mapa spotřeby přebírá skutečné lokální ceny vybrané reprezentace. Test hlídá,
  že její zapnutí nemění paket, její bity dávají konzistentní součet a krátký
  model nezanechá cenu původního modelu. Souřadnice mapy jsou v pracovním
  rozlišení; UI správně roztahuje celou mapu přes celý rekonstruovaný obraz.
- Rekurzivní RGB filtr transponuje čtvercové dlaždice na místě: tři floatové
  roviny místo šesti. Výsledek této optimalizace zůstal pixelově stejný.
  Dlaždice, jejichž celý vliv později přepíše přesná mapa plochých barev,
  se nefiltrují. Konzervativní okraj zahrnuje podporu interpolace; nezávislý
  test porovnává výstup s/bez vynechávání a s různým počtem vláken.
- Odstraněny opakované kontroly pracovníků/RAM při vnořeném dekódu a další
  skeny již známé neprůhlednosti. Nulová barevná predikce přeskočí čtení jasu.
  rANS odmítá prokazatelně nevýhodný nekompaktní model před jeho konstrukcí.
- CLI měření do připraveného bufferu již nealokuje tento buffer uvnitř
  časované části. `measure-decode` měřilo připravený buffer správně i předtím.
  Nové `compare_decode.py` střídá binárky nad totožnými starými pakety;
  rychlost nového encodu nelze odvozovat z tohoto porovnání dekodérů.
- Opraveno předání příznaku tolerance zvonění do společného rozpoznávání hran.
  Nové silnější odbarvování kontur se **nepřijalo**: malý zisk na postavě
  doprovázela regrese přírodní textury. Nepřijaly se ani silnější adaptivní
  kvantizace, prořezávání izolovaných koeficientů a dražší lokální afinní filtr.
- Finální kontrola: 5 scén × 30/80/150 kB, každý soubor pod limitem, skutečné
  PNG a SSIMULACRA2 v `diagnostics/R460-next/final-review` a `final-holdout`.
  Na 30/80 kB se skóre zlepšilo v 9 z 10 případů; pes 30 kB -0,038 bodu.
  Všech 5 výsledků 150 kB je pixelově shodných s výchozí R460.
  RGB a okolí hran nemají všude nižší chybu; skóre není důkaz odstranění lemů.
  Viditelný posun je mírný, 30kB obraz má dál výrazné ztráty a artefakty.
  Celý CPU decode není zaručeně pod 16 ms, zvlášť v p95 a při souběžné zátěži.

---

# Aktuální architektura R450 — 16. 10. 2026

Veřejný profil je jediný: R450 (interní ID 56 a rodina paketů R441). Přímý enkodér rozhoduje o pracovním
rozlišení a barevném modelu podle hustoty dat, poté kóduje waveletové reziduum.
Přesné ploché barvy mají mapu, zbytek zpracuje současná rekonstrukce artefaktů,
zvětšování a schválený Skirt/dering + tangenty + AA + ochrana textu.

`encode.cpp` již není router stovek experimentů. `color_prediction.cpp` má pouze
současný centrovaný barevný model. `wavelet_profile.hpp` obsahuje vybrané parametry
aktuálního jádra a jeho interního pilotu, bez historického katalogu experimentů.
`artifact_cleanup`, `side_cleanup`, `edge_model` a `edge_upscale` jsou používané
součásti R441, proto zůstaly a mají jména podle funkce.

Dekodér přijímá pouze aktuální rodinu komponent: 130 (R441), 129/mode 12
(základní rekonstrukce), 77 (přesné plochy), 28/mode 130 (pracovní rozlišení),
55 (barevná predikce), 114 (nativní CDF 9/7) a 4 (přesná RGBA/paleta).
To nejsou samostatně nabízené staré verze. Jiná historická schémata se odmítají.
Vnořené pakety jsou validované pohledy do zdrojových dat, bez kopií payloadu.
Vlastněný i přímý RGBA výstup používají stejný dispatch a rekonstrukci.

Binární range aritmetika je jediná v `binary_range.hpp`. Pomocný rANS a slovník
jsou odlišné algoritmy pro jiný druh dat; jejich nucené sloučení by samo o sobě
nezaručilo menší soubory. Kvantizace, modely a streamové konstanty jsou zachované.

Před odstraněním historie se ověřila původní auditní tvrzení. Důležité korekce:
počáteční pravděpodobnosti nejsou ve všech režimech stejné; pět ze šesti údajně
nepoužívaných hlaviček mělo skutečné použití; pět bajtů range finále již obsahuje
prefix; všechny historické filtry se nespouštěly v řadě. Dvojí dekódování pro
výběr staré mapy obnovy nebylo na aktuální cestě. Tato stará větev včetně možnosti
nekonečného opakování masky byla nyní odstraněna celá.

Aktualizace rychlé CPU rekonstrukce, 16. 10. 2026:

- Rekurzivní filtr používá dlaždice 96×96 s halo 12, SIMD transpozice a znovu
  používá váhy pro zpětný průchod. Směrové vzorkování podél RGB tangenty se zpracuje ještě před
  kvantováním RGB. U nízkého toku se čistí pracovní rozlišení; ochrana textu
  předchází zvětšení s korekcí přechodu omezenou barvami sousedních vzorků.
- Při 30 kB / 1440p se používá rekurzivní čištění a směrová tangenta. Kolem 80 kB je multiplikátor
  čištění 0,8 místo 0,5 a mírně silnější kontrast přechodu. Od 80 do 150 kB
  následuje hladký útlum k nule. Starý průchod u tří 150kB snímků měnil jen asi
  1–2 % pixelů, většinou o jediný odstín, za několik ms. Vypíná se dodatečná
  R441 rekonstrukce; základní obnova waveletů a barev zůstává aktivní.
- GPU není součástí produkčního dekódu. UI recykluje nejvýše dva RGBA snímky
  po uvolnění posledního vlastníka. Na dosud zobrazený snímek se nezapisuje.
  Cache pracovních/výstupních bufferů mají retenční limit 32 MiB na buffer.
- Specializace aktuálního entropického režimu, větší pásy inverzní transformace
  a přesná AVX2 gamma tabulka zkracují dekódování. Enkodér předává jednou
  ověřenou neprůhlednost přes vlastní vrstvy místo opakovaných průchodů RGBA.
- Adaptivní kvantizace nízkého toku adaptive=3 byla vyzkoušena a odmítnuta:
  tři kontrolní scény měly horší SSIMULACRA2 bez přesvědčivého vizuálního zisku.
  Formát a přídavná data se nemění. Méně artefaktů samo o sobě neznamená nový
  detail, který se při kompresi ztratil.

Zmrazené staré pakety/hashe v samples/regression a obrázky diagnostics/current
zůstávají referencí a nesmějí se přepsat kandidátem. Rychlá rekonstrukce úmyslně
mění pixely; starý RGBA hash není očekáváním nového výsledku. Nové záznamy patří
do diagnostics/R441-speed. check_fast_decode.py kontroluje rozpočet, shodu
vlastněného/přímého výstupu a počtů vláken. --baseline vyžaduje téměř přesnou
pixelovou shodu a je vhodné pouze pro optimalizace bez zamýšlené změny vzhledu.

Testy: kontejner, entropie, RGBA/paleta, alfa, rozpočty, metadata, poškozené
pakety, hlídané buffery, vypnutí filtru, SIMD konce a vlákna. Navíc se kontroluje
snížení zubatosti syntetické kontury, zachování rovné hrany, monotónní útlum síly
při stejné hustotě dat, přesnost gamma převodu a životnost sdílených UI bufferů.

Měření buffer zahrnuje celý decode a filtry do připraveného RGBA výstupu;
owned navíc zahrnuje alokaci výsledku. Ani jedno nezahrnuje disk, PNG export či
vykreslení UI. Čísla jsou v R441-speed/final-cpu-review/results.json. 16 ms není
záruka pro všechna data a zatížení; prvotní alokace a souběžná zátěž mohou čas
výrazně zvednout. Regrese se neposuzuje pouze podle SSIMULACRA2.

R450, závěrečné změny:
- Nové nízkotoké pakety vypínají přídavné směrové čištění nativních rovin.
  Hrubé čištění uvnitř transformace se vynechá pod 0,015 cílového bajtu/pixel
  zdrojového obrazu; při střední hustotě zůstává. Staré pakety zachovávají své
  uložené parametry. Uvedené rychlostní zisky proto vyžadují nový encode.
- Tangenta používá čtyři vzorky ±2/±4, společnou RGB bránu a konvexní směs.
  Nemění polohu kontury žádným uloženým geometrickým modelem.
- Inverzní CDF slučuje poslední dvě vertikální lifting operace a přímý zápis
  prokládaného výsledku. Test proti nezávislému skalárnímu výpočtu pokrývá
  okraje, úzké pásy a SIMD konce. Šest původních obrazů zůstalo pixelově shodných
  při samostatném zavedení této optimalizace.
- U nových paketů R441 hlavička kvality vychází ze skutečné velikosti dat,
  nikoli z nedosaženého rozpočtu. Uložení nemá další bajtovou režii.
- Menší pracovní rozlišení 1920 místo 2080 a širší lokální křivost byly
  odmítnuty kvůli zhoršení dalších scén. Kompresní poměr se tímto releasem
  nesnižuje; zlepšuje se rekonstrukce při stejné velikosti.
- UI zobrazuje R450, zachovává stabilní názvy nastavení a interní ID formátu.
  Dva původní posuvníky jsou v doplňkovém čištění, výchozí 0 %. Skirt 100 %,
  Tangents 0 %, AA 50 %, kontrast 0 %, ochrana textu zapnutá.
- Finální přeznačení a balení R450 proběhlo s -SkipTests na přání uživatele;
  předchozí výsledky se nesmějí vydávat za nové ověření finálního balíčku.
