# Audit architektury Fajk Studio — 28. 10. 2026

## Stav po opravách v development větvi

Původní nálezy níže popisují stav na začátku auditu. Aktuální zdroje již
obsahují tyto opravy; zveřejněná R570 a její SDK se nepřepisují:

- Vlastněné DecodeSession místo thread-local referencí, společný rozpočet
  hlavních cache a trim mezi voláními. Střídání vlastníků i migrace vlákna
  jsou pokryté regresí. Zbylá TLS pracovní paměť není celkový RAM limit.
- Residentní GPU upscale + finální ostření s jedním RGBA readbackem.
  Zachovává přesný obraz původního GPU upscale + CPU ostření.
- Lokální lossless opravy nativní reference (motion v137/mode3), znovu
  ověřené všemi lokálními i globálními quality gates a bajtovým limitem.
- P návrh soutěží proti skutečné přesné koeficientové alternativě.
  Preview/commit/discard brání použití nezapsaného snímku jako reference.
- Omezená nezávislá příprava dalšího intra v Max; sdílí celkový počet
  vláken a paměťový limit. Pořadí, délka referencí a atomický zápis zůstávají.
- Playback služba byla oddělena od XAML, včetně front, zrušení, readerů,
  codec relace a poolů. DCI 4K pool už nenaráží na jednotlivý limit 32 MiB.
- Další hold regrese: seek zpět na předka aktualizuje identitu holdu.
- Automatická chroma správně používá pro samostatné fotky 4:4:4; explicitní
  výběr má přednost. Původní implementace používala 4:2:0 i u fotek.
- Statistiky převodu sčítají compute práci přípravných workerů i calleru.
  Samotné gpuActivity() je čítač caller threadu, nikoli celého procesu.

Pokusy o lazy rekonstrukce byly odstraněny: přes shodné pakety zpomalily
skutečný export o přibližně 2–5 %. Paralelní čtení entropických streamů
je omezeno na větší neodvozené vstupy; malé skeletony a již zachycené
encode koeficienty nepřidávají další plánování. Jeho samostatný výkon se
nesmí vydávat za prokázané další zrychlení celého videa.

Klipový kvantizér je experiment a standardně vypnutý. Původní stabilizace
šetřila data za vyšší barevnou chybu; samotné snížení flickeru nestačí.
Nová varianta vyžaduje zaplnění rozpočtu a samostatné zdrojové ověření.

Stále zbývá plný GPU surface/presentation řetězec přes schválené základní
kontury, odstranění plné intra oracle a více nedávných referencí. Nejsou
označeny jako opravené. Výsledky a přesné logy: CURRENT.md a
diagnostics/ArchitectureFix-20261028/. Testy nyní zahrnují 11 sad;
veřejné SDK ověřuje 1 620 decode kombinací včetně mode3, holdů, seeků,
nastavení, paddingu, guardů a zotavení po chybě.

Rozsah: aktuální zdroje po R570, převod vstupu, intra/koeficientový/pohybový
kodek, GPU rozhraní, plánování CPU, reference, filtry, přehrávání a ověřování.
Nejde o důkaz nepřítomnosti dalších chyb. Níže jsou oddělené chyby,
architektonická omezení a hypotézy vyžadující měření. Vydaná R570 se nemění.

## Hlavní závěr

Největší prostor není v prostém přidávání vláken nebo přesunu jednotlivých
filtrů na GPU. Současná rozhraní nutí CPU a GPU střídavě čekat a předávat si
celé obrazy. Max komprese je navíc postavená kolem úplné nezávislé komprese
každého změněného snímku a rozhodnutí přijmout/zamítnout celou pohybovou
alternativu. To vytváří vysokou dolní mez práce a omezuje stabilitu i úsporu.

## P0 — přenosy a synchronizace mezi GPU operacemi

**Jisté ze zdrojů:** `Frame` vlastní `vector<uint8_t>` RGBA v RAM;
`GpuPlane` je hostitelský `span<float>`. GPU nativní dekódování vrací RGBA
přes staging buffer a čekání. Následující GPU kontury/upscale přijímají znovu
hostitelský `Frame`. UI pak kopíruje obraz do `WriteableBitmap`.

Zdroj: `include/fajk/types.hpp`, `src/gpu_compute.hpp`,
`Engine::decodeNative`, `Engine::contourUpscale` v `src/gpu_compute.cpp`,
`Studio::bitmap` v `src/ui/studio.cpp`.

**Dopad:** jednotlivé části jsou GPU akcelerované, ale celý řetězec není
residentní na GPU. Rozhraní vynucuje některé zbytečné readback/upload a fence
body. Ne každý stávající přenos je zbytečný: CPU entropie a veřejné CPU SDK
nějakou hostitelskou reprezentaci potřebují.

**Směr opravy:** vlastněný GPU surface/plane handle, jasné vlastnictví a
životnost, jeden plán pro inverse → nativní opravy → upscale → finální
ostření → prezentaci. CPU materializace pouze na skutečné hranici exportu
nebo CPU SDK. Nejprve propojit dvě sousední fáze, ověřit pixely, teprve pak
měnit další. Samotný počet odstraněných kopií nezaručuje konkrétní ms.

## P0 — úplný intra encode před každým pohybovým pokusem

**Jisté:** `MotionTemporalEncoder::encode` nejprve volá `codec->encode`,
`readMotionRoot` a nezávislý `codec->decode`. Teprve pak zkouší predikci,
reziduum a kontrolu jeho kvality. I výborně předpověditelný změněný snímek
tedy platí za hotový samostatný obraz. Zachycené koeficienty šetří entropické
dekódování; neodstraňují veškerou transformaci, kvantování a rekonstrukci.

**Proč to vzniklo:** nezávislý obraz je bezpečná alternativa a kvalitativní
oracle. Tuto ochranu nelze jen vypnout a považovat rychlost za hotový úspěch.

**Směr:** oddělit přípravu zdroje, vyhledání pohybu, výběr režimu a samotné
kódování. Levný zdrojový test/scénový střih před plným intra; sdílení
analýzy a transformovaných dat; později bloková intra alternativa pro
problémové oblasti. Ověřit úsporu při stejné kvalitě vůči úplné alternativě.

## P0 — znovu rekonstruovaná kanonická a zobrazovaná podoba

`readMotionRoot` explicitně volí CPU kvůli deterministické referenci.
Následující nezávislý decode používá zvolený backend. Systém tak může
rekonstruovat tentýž transformovaný obraz různými cestami. Oddělení
predikčního obrazu od ostřeného/upscalovaného zobrazení je správné a chrání
před driftováním; duplicitní výpočet je cena současného provedení.

**Směr:** jeden rekonstrukční plán se dvěma odběry výsledku, kanonickou
referencí a prezentací. Zachovat přesná pravidla zaokrouhlení a filtrů.
CPU a GPU jsou u některých intra operací jen velmi blízké, nikoli vždy
bitově shodné; jejich záměna vyžaduje ověření uzavřené referenční smyčky.

## P0 — lokální závada zamítne celou pohybovou alternativu

`compareTemporalQuality` vyhodnocuje lokální dlaždice i globální chyby.
Jediná poškozená dlaždice zamítne celý P návrh. Encoder následně použije
koeficientové překódování celého samostatně zakódovaného snímku.
Kontrola správně brání barevným blokům, ale chybí lokální oprava.

**Potvrzený rozsah na uložených 240 snímcích Fortnite:**

- 80 kB / Max 14: 8 samostatných, 194 koeficientových, 38 pohybových.
- 30 kB / Max 6: 6 samostatných, 206 koeficientových, 28 pohybových.

Koeficientová komprese je časová úspora dat při nezměněném intra obrazu.
Nezajišťuje jeho časovou stabilitu. Počet P snímků sám o sobě neurčuje
celkovou úsporu; rozhodují jejich skutečné bajty.

**Směr:** prostorově lokální režim: skip, motion+residual, lokální intra,
případně oprava barev. Výběr podle bitů i zkreslení s lokálními limity.
Definovat okraje filtrů tak, aby nevznikaly nové švy. Jde o změnu návrhu
bitstreamu/decoderu, nikoli bezpečnou jednorázovou úpravu prahů.

## P1 — časová stabilita není součástí nezávislého rate control

Intra cesta znovu rozhoduje o rozpočtu, kvantizéru a koeficientech každého
snímku. Prahové zapínání slabých detailů, úpravy kroku při dorovnávání
velikosti a případné změny pracovní geometrie mohou měnit vzhled i při
malé změně zdroje. Deterministický výpočet nepředstavuje časově stabilní
kvantování. Pozorovaný šum proto automaticky neznamená náhodný seed.

**Důkaz omezení:** R570 při nízkých rozpočtech nezlepšila všechny flicker
metriky; přesné výsledky a horší případy jsou v `CURRENT.md`. Ze samotného
zdroje nelze připsat konkrétní podíl každé příčině.

**Směr:** stav rate control na úrovni klipu, omezení kolísání kroku a
geometrie, jemná hystereze při výběru detailů, časová cena změny pouze ve
zdrojově/pohybově stabilních oblastech. Na střihu a odhalení nových ploch
stav resetovat. Nepřimíchávat slepě předchozí obraz — to by vracelo ghosty.

## P1 — malá sada referencí a tvrdé restarty řetězců

Encoder používá aktuální referenci nebo původní root; po hloubce 15 může
přeskočit k rootu. To omezuje délku seeku a je záměrná ochrana. Chybí však
výběr mezi více nedávnými dobře predikujícími referencemi nebo rozumný
checkpoint. Starý root může být po pohybu kamery drahý a vynutit fallback.

**Směr:** 2–3 explicitní reference s cenou bajtů, chyby a délky decode.
Zachovat maximum pěti sekund pro změněný obraz, levné přesné holdy a
omezenou závislost. Nezvyšovat prostě délku řetězce; zhoršilo by to seek.

## P1 — globální GPU engine a synchronní rozhraní

GPU operace sdílejí jeden `Engine`, immediate context, pracovní buffery
a mutex. Některé callbacky CPU práce běží při drženém GPU zámku.
To je správná ochrana sdílených prostředků, nikoli samo o sobě race bug,
ale omezuje překrývání nezávislých snímků a zvyšuje vazbu mezi operacemi.

**Směr:** vlastněné prostředky rozpracovaných úloh a omezený ring staging
bufferů; rozlišit submit a čekání; CPU entropii další úlohy překrývat s GPU
zpracováním aktuální. Odstranění mutexu bez přestavby zdrojů by bylo chybné.

## P1 — plánování podle režimů místo skutečných závislostí

`pipeline.cpp` nastavuje u Max a podobných oblastí `frameWorkers=1`.
Pohybová reference vyžaduje pořadí, ale některé analýzy zdroje a příprava
nezávislých podkladů mohou být na příštím snímku nezávislé. Současné
rozhraní je neumí odděleně plánovat. Vstupní `MediaSource::next` je také
synchronní součást převodní smyčky.

**Směr:** omezený pipeline graf: vstup → analýza → závislá reference →
entropie → zápis; oddělit latenci jednoho snímku od průchodnosti videa.
CPU/GPU fronty musí sdílet celkový limit vláken a paměti. Přidání dalších
celých encoderů může zhoršit paměťovou propustnost i GPU serializaci.

## P1 — skrytý stav v thread_local cache

Reference, koeficienty, pracovní obrazy a některé encode/decode kontexty
jsou rozdělené do několika `thread_local` objektů. Dva decodery na jednom
vlákně si mohou vytlačovat cache; stejný decoder na jiném vlákně o ně
přijde. Paměť se násobí počtem aktivních vláken. Existující lokální limity
jsou užitečné, ale netvoří jeden rozpočet celé relace.

**Směr:** explicitní `DecodeSession`/`EncodeSession` s vlastními referencemi,
pracovní pamětí a statistikami. Thread pool má poskytovat pracovníky,
nikoli určovat vlastnictví stavu video relace.

## P1 — chyba prvního holdu, nyní opravena

`rememberTemporalKey` původně nezapamatoval právě zobrazený závislý
koeficientový nebo pohybový snímek. První bezprostřední přesný hold proto
znovu rekonstruoval předka. Regresní test nejprve prokazatelně selhal.

Oprava používá informaci z indexu o následujícím osmibajtovém holdu.
Hotová prezentace se uloží u příslušného předka; běžné pohyblivé snímky
bez následujícího holdu nedostávají další kopii. Kontroluje se identita,
obsah, rodič, velikost, filtr, backend a pracovní rozpočet; ostření se
aplikuje až nad uloženou neostřenou prezentací. Formát na disku se nemění.

Testy pokrývají první hold po koeficientovém i pohybovém snímku, CPU/GPU,
filtry, ostření a čerstvý seek. Přidán je nativní obraz nad GPU prahem,
který výslovně kontroluje počet skutečných GPU dekódování.

Ověření v `diagnostics/Audit-20261028/`: regresní selhání je v
`reproduce.log`, finální úplná sada v `final-checks.log`. Pomocný nástroj
`tools/make_repeat_probe.py` vložil za 80 původních paketů 80 přesných holdů
bez rekomprese a při zachování časové osy. Dva párové průchody na CPU i
GPU zachovaly každý pixel, PTS/duration, buffer guardy a vybrané fresh seeky.
Časy tohoto průchodu byly neobvykle vysoké a chybí průběžný záznam zátěže;
nepoužívají se jako tvrzení o běžné rychlosti. Důkaz odstraněné práce
poskytuje i přímo regresní test bez dalších coefficient/motion fází.

## P1 — mezery v měření a testech

- Některé menší fixture záměrně běží i v GPU režimu přes CPU fallback;
  samotné nastavení `backend=Gpu` nedokazuje použití GPU. Existuje i větší
  samostatná GPU sada, která aktivitu skutečně ověřuje. Nová hold regrese
  doplňuje právě tento chybějící konkrétní případ.
- Dosavadní hlavní herní měření je jeden Fortnite klip. To nestačí pro
  obecná tvrzení o všech hrách, poměru komprese nebo optimálním počtu vláken.
- Časy caller threadu zahrnují frontu, čekání a CPU práci; některé fáze jsou
  vnořené. Chybí úplná samostatná GPU timestamp timeline a dlouhodobý
  rozpad latency/throughput. Vnořené časovače se nesmějí sčítat.
- Krátké testy už několikrát ukázaly domnělé zrychlení, které delší test
  nepotvrdil. Odmítnuté experimenty jsou popsané v `CURRENT.md`.

**Směr:** malý stabilní korpus: statika s malou změnou, HUD/text, gradienty,
pan kamery, částice/tráva, prudký pohyb, střih, různé rozlišení a VFR.
Sledovat wall time, CPU práci, GPU časy, přenosy, alokace, p95/p99,
velikost, lokální barevnou chybu a motion/stable flicker. Kvalita a seek
musí být měřeny se stejnými decode volbami jako běžné používání.

## P2 — složitá konfigurace a smíšené odpovědnosti

`trialPredictive` přijímá dlouhou řadu boolů a parametrů. `wavelet.cpp`
obsahuje mnoho historických experimentálních větví a formátových flagů.
`studio.cpp` propojuje UI, práci se soubory, přehrávání, debug a smoke testy.
To zvyšuje cenu změn a riziko nechtěných kombinací. Odstranění konstantně
mrtvých větví samo o sobě nemusí zrychlit release — překladač je optimalizuje.

**Směr:** typované `MotionConfig`, `ResidualConfig`, `DisplayConfig`,
validace na hranicích; oddělený playback controller a presentation backend;
jedna specifikace kanonické rekonstrukce sdílená kontrolou kvality i decode.
Refaktorovat po ověřitelných částech s existujícími výstupními oracle.

## Co nepovažovat za chybu bez dalšího důkazu

- 50 % GPU a 50 % CPU neznamená automaticky možnost dvojnásobné rychlosti.
  GPU má odlišné enginy; čekání a sériové závislosti mohou omezovat latenci.
- Použití D3D11 compute není samo o sobě důkaz zastaralého nebo pomalého
  kodeku. Přechod API neodstraní nevhodné datové rozhraní a kopie.
- Více filtrů nebo jejich plošné zesílení není automaticky vyšší kvalita.
  Ztrátu barevných detailů a ghosting je nutné hodnotit vedle noise.
- Tolerance podobnosti není úplná náhrada motion compensation a rezidua.
  Její prosté zvýšení může dát velkou úsporu za viditelné lokální chyby.

## Doporučené pořadí přestavby

1. Dokončit malé korektnostní opravy a regresní pokrytí referencí/holdů.
2. Explicitní relace a GPU surface rozhraní; propojit sousední decode fáze.
3. Oddělit zdrojovou přípravu od rozhodování a plné intra komprese v Max.
4. Lokální opravy pohybové alternativy a stabilnější rate control klipu.
5. Pipeline napříč snímky s omezenými frontami; ladit podle delšího korpusu.

Tyto kroky jsou návrh, nikoli závazek konkrétního zrychlení či 60% úspory.
Kvalita, velikost a čas tvoří společný kompromis; je nutné udržovat několik
měřitelných profilů místo prohlášení jednoho nastavení za absolutní maximum.
