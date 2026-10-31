# Nástroje současného kodeku

`verify_codec_revision.py --reference OLD_CLI --output DIR` střídá dvě
verze na stejných zdrojích, rozpočtech a backendech. Hlídá přesnou shodu
celých paketů i RGBA a žádné chybové GPU fallbacky. `--reuse` obnoví hotové
průchody pouze se stejnými hashi binárek, počtem opakování a pracovníků.
`verify_decode_revision.py --reference OLD_CLI --inputs FILE.fajkv --output
DIR` měří samotné dekodéry nad stejnými pakety, výchozí rozpočty CPU jsou
1/2/4/10 vláken. `--workers`, `--repeats`, `--rounds`, `--backend` mění rozsah.
Výstupy zahrnují přesné pixely, čas, práci CPU, profily a GPU aktivitu.
Nový čítač CSV `gpu_contour_filters` rozlišuje skutečné dočištění na GPU
od výběru rychlejší CPU cesty; kapacitní volba CPU není chybový fallback.

R470 GPU: `verify_gpu.py --output DIR` střídá CPU/GPU nad třemi referencemi
a limity 30/80/150 kB. `--inputs`, `--budgets`, `--rounds`, `--repeats` a
`--workers` mění rozsah. Používá stejné R470 volby, celý soubor a připravený
RGBA buffer. CSV obsahuje čas, procesní CPU čas, cykly, skutečnou GPU aktivitu,
přenesené bajty a návraty na CPU. CPU čas je součet všech vláken procesu,
není to spotřeba energie. Referenční SSIMULACRA2 a křížový decode hlídají
kvalitu a kompatibilitu. `--reuse` znovu ověří uložená měření bez nového měření.
Izolované rozdíly po diskrétních konturových filtrech mají samostatný přísný
limit průměru i podílu změněných kanálů. Raw GPU testy používají těsnější meze.
Měření spouštějte bez souběžného buildu/ASAN testů. CLI `encode`, `decode`,
`measure`, `measure-decode` přijímá `--gpu` / `--cpu`; `hardware` uvádí kartu.
Nativní test `gpu_compute` ověřuje skutečnou aktivitu GPU, CDF, filtry,
chroma režimy, zdrojové klasifikace a sdílené buffery. Bez vhodného adaptéru
se přeskočí, chyba kompilace shaderů test nezelení. Kernely se kompilují
nástrojem `fajk_gpu_shader_compiler` při sestavení a jsou vložené do binárek.

R464: `review_flat_allocation.py --exe PATH --output DIR` kontroluje skutečné
encode/decode pro 0/50/75/100 % při stejných celkových limitech 30/80 kB.
`--samples`, `--budgets` a `--settings` rozsah omezují. Pevné ROI ve zdrojových
souřadnicích odlišují chybu stěn, postavy, textu a oken. CSV `*-tiles.csv`
z `inspect-allocation` uvádí náklady entropy dlaždic před jejich rozprostřením
do mapy, včetně složky, pásma a skutečné RDO váhy. Report uvádí i náklady
dlaždic celým footprintem obsažených ve výřezu; nejde o přesné vlastnictví
bitů po pixelech a širší vliv waveletové báze může přesahovat footprint.
Tyto diagnostické encode časy se neměří jako běžný výkon kodeku.

`compare_encode.py --flat-allocation 100` umožňuje střídavě měřit shodné
nastavení obou verzí. Výchozí je 50. Aktuální vydání a jeho důkazy jsou v
`diagnostics/R464-final-20261018`; `R462-flat-audit-20261018` obsahuje také
nepřijaté pokusy, nikoli alternativní doporučené profily.

`compare_sdk_work.py --before OLD_DLL --after NEW_DLL --packets DIR --output
RESULTS.json` střídá CPU dekodéry nad stejnými kompatibilními pakety. Výchozí
32 párů na snímek lze změnit přes --repeats. Zapisuje celý decode včetně
filtrů do připraveného RGBA bufferu, p95, procesorové cykly, procesní CPU čas
a kontrolu guard bajtů. --workers a volitelné --before-workers/--after-workers
umožňují ověřit plánování vláken. Čítače se čtou jen v tomto nástroji, nikdy
v produkčním kodeku. Cykly doplňují čas, nezaručují nezávislost na frekvenci
a zátěži. Nástroj sám nepotvrzuje vizuální kvalitu ani nový encoder.

Audit R462: `audit_r462_controls.py --sdk PATH_TO_DLL --cli PATH_TO_EXE
--output DIR --expect-fixed` vyžaduje funkční SDK ovladače, samostatný rozsah
AA při vypnutém Skirt a úspěšný automatický encode64×64. Bez --expect-fixed
jen zaznamená stav staršího buildu. Mění pouze diagnostické kopie paketů.
`compare_encode.py` má navíc --max-edge-mae-increase a --max-roi-mae-increase;
ROI kontroly samostatně hlídají text, obličej, tmavé vlasy a další geometrii.
Přijetí změny vyžaduje předat limity, uložit výřezy a zkontrolovat je.

`fajk_cli inspect-frame INPUT PREFIX [LEFT=0] [RIGHT=2] [GAIN=1] [FRAME=0]`
vytvoří `PREFIX-debug.png` a `PREFIX-debug.txt`. ID pohledů v pořadí:
0 finální RGB, 1 nativní RGB před konturami, 2 jas Y, 3/4 rekonstruované
C1/C2, 5/6 uložené barevné složky / rezidua před predikcí, 7/8/9 wavelet
koeficienty Y/C1/C2, 10/11 sklony barevných modelů, 12 základní kontury,
13 přesné plochy, 14 alfa, 15 zakódované bajty. UI navíc nabízí 16 skutečnou spotřebu dat,
17–19 AQ váhy Y/C1/C2 a 20 klasifikaci hladkých ploch; ty vyžadují
diagnostický encode ze zdroje a nejsou uložené v paketu. GAIN 1–16 zesílí
diagnostické hodnoty kolem šedé 128; koeficienty mají podepsanou asinh škálu.
Chybějící vrstva dává výslovně označený nedostupný pohled. Bajtové offsety
jsou uvnitř snímkového paketu, bez kontejneru. Proudy a hlavičky se sčítají
bez dvojího účtování vnořených obalů. Export PNG/reportu není měření dekódu.
CLI `encode` podporuje `--420`, `--422` i `--444`, stejně jako UI pod kvalitou.

Iterace 17. 10. 2026: `compare_encode.py --before OLD_EXE --after NEW_EXE
--output DIR` střídá celé encode/decode při stejných limitech kontejneru.
Měří výpočet bez startu procesu a PNG IO, ukládá median/p95, velikosti,
SSIMULACRA2, RGB/edge-band chybu, shodu pixelů a skutečné výřezy. Parametry
`--samples`, `--budgets`, `--rounds`, `--repeats` omezují pokus;
`--max-ss2-loss 0.2` vyžaduje kontrolu každé scény po uložení výsledku.
Spouštějte měření sekvenčně, bez souběžného buildu nebo dalšího benchmarku.

CSV `inspect-contours` nyní pro `color/geometry/thin` používá výsledná
vodítka po regularizaci, sdílená s produkčním rendererem a `-effect.png`.
Položky `broad_*`, `fine_*` a jejich důvody stále popisují místní mezikroky.

`check_fast_decode.py` měří CPU decode včetně filtrů, ověřuje rozpočet,
pixelovou shodu mezi owned/buffer a jedním/více vlákny. Potřebuje numpy/Pillow.

```powershell
python dev/tools/check_fast_decode.py --packets dev/diagnostics/R441-speed --output dev/diagnostics/R441-speed/check --repeats 40
python dev/tools/review_cleanup_rates.py --packets dev/diagnostics/R441-speed --output dev/diagnostics/R441-speed/rates --repeats 20
```

`buffer` vynechává prvotní alokaci RGBA, `owned` ji zahrnuje. Oba měří celý
dekódovací výpočet. Parametr --baseline je pro optimalizace bez změny vzhledu:
vyžaduje téměř pixelovou shodu, nikoli nově navrženou rekonstrukci.

`review_cleanup_rates.py` používá identická komprimovaná data pro zapnutý a
vypnutý filtr; mění pouze uložené nastavení a opravuje kontrolní součty. Zdroje
nepřepisuje. Chybějící 80/300kB případy vyrábí z uchovaných PNG referencí.

`check_current_revision.py` je historická kontrola vůči pomalé referenci před
CPU rekonstrukcí. Její RGBA shoda se u rychlého dekodéru neočekává. Píše do
`diagnostics/legacy-regression-check`, nikdy nepřepisuje staré hashe ani obrázky.

CMake target `fajk_metric` používá referenční SSIMULACRA2 knihovny JPEG XL.
Nejsou backendem kodeku. Čísla doplňují vizuální kontrolu, nenahrazují ji.
`Build.bat` spouští aktuální testy. Staré diagnostiky byly výrazně pročištěny;
nástroje se mají opírat o samples/references a samples/regression.

`review_cpu_revision.py` reprodukuje nové encode 30/80/150 kB, celkový CPU
decode s vlastním/připraveným výstupem a vypnutým R441 filtrem. Měří sekvenčně,
exportuje skutečné výřezy postavy, obličeje a herní scény a SSIMULACRA2.
`--require-score-gain` vyžaduje >0,2 bodu na 30/80 kB a žádnou významnou
regresi na 150 kB. Obrázky musí být posouzeny zvlášť, skóre není schválení.


R460: `review_r460.py --exe EXE --output DIR [--baseline BASELINE_DIR]`
porovnává schválené hodnoty (Skirt 50, kontury 200, plochy/Tangents 0).
Podporuje --samples a --budgets. Exportuje PNG výřezy, skutečné velikosti,
SSIMULACRA2 a median/p95 celého CPU decode. Baseline spusťte se zachovaným
R450 CLI do samostatného adresáře, potom kandidáta se stejnými zdroji.

`--max-ss2-loss 0.2` zastaví kontrolu při regresi jednotlivého snímku;
výsledek uloží i před selháním. Doplňkové RGB/edge-band chyby a maska neutrálního
UI textu pomáhají odhalovat kompromisy, nejsou samostatným měřítkem vzhledu.

`compare_decode.py --before OLD_EXE --after NEW_EXE --packets DIR --output DIR`
měří oba dekodéry střídavě nad **stejnými kompatibilními pakety**, ve třech
kolech. Ukládá i mediany jednotlivých kol a p95, aby proměnlivá zátěž nebyla
zaměněna za optimalizaci. Nevyžaduje shodu pixelů, proto sám nepotvrzuje kvalitu.

`fajk_cli inspect-compression INPUT.fajk` porovná přesně ekvivalentní
reprezentace kvantovaných koeficientů a vypíše CSV s jejich cenou. Vyžaduje
4:2:0 paket s modelem 23 nebo 31 bez adaptivní přesnosti; na každém pásmu
kontroluje shodu dekódovaných celých koeficientů. Nezahrnuje kontejner a metadata.
Nový model 31 může zvolit i selector 7 (sedm základních úrovní magnitudy).
Staré dekodéry tuto novou volbu neumějí; při porovnání SDK použijte pakety,
kterým rozumějí oba porovnávané dekodéry.

Aktuální výsledky této iterace: `diagnostics/R460-next/final-review` a
`final-holdout`. Adresáře `shared-dering`, `edge-*`, `local-*`, `aq-*` apod.
obsahují pokusy, nikoli schválený profil. Starší historická diagnostika byla
pročištěna; pro nové kontroly používejte uchované PNG v `samples/references`.

Oprava barevného lemu: schválený omezený pokus je
`diagnostics/R460-halo/colored-skirt-wide`, doplňkové scény v `final-holdout`.
Ostatní varianty v `R460-halo` jsou nepřijaté experimenty. `paired-speed`
porovnává oba dekodéry na totožných paketech, `final-decode-check` kontroluje
shodu výstupních cest a vláken.

`review_silhouette.py --reference ORIGINAL --before OLD --after NEW --output DIR
--box X0 Y0 X1 Y1 --background-color R G B --tolerance 80` měří oba okraje
siluety v pásech 3/8/16 px. Segmentační maska pochází **pouze z originálu**,
je pro oba kandidáty stejná a ukládá se jako mask.png. Barvu i výřez musíte
explicitně zadat podle scény; nejde o univerzální metriku ani rozpoznávání
objektu v produkčním dekodéru. comparison.png je skutečný dekódovaný výřez
zvětšený přesně 2× metodou nearest-neighbor.

Diagnostika pokrytí kontur: `fajk_cli inspect-contours INPUT.fajk PREFIX 8`
uloží pracovní obraz před rekonstrukcí a CSV důvěry barev/geometrie/tenkých
tahů. `review_contour_guides.py PREFIX --box X0 Y0 X1 Y1 --scale 3` vykreslí
tyto skutečné hodnoty s bilineární interpolací jako v rendereru. Souřadnice
výřezu jsou v uloženém pracovním obrazu, který může být menší než originál.
Nejde o segmentaci objektu ani skutečnou výslednou váhu korekce každého pixelu:
tu dále omezuje vzdálenost od hrany, shoda barev a ostatní ochrany.

Konturový debug nově exportuje také `PREFIX-effect.png` přímo z výpočtu váhy
rendereru: zelená = korekce tvaru, červená = oprava barevného okolí kontury.
Není to celkový Skirt/Tangents filtr ani rozdíl hotových 8bitových obrazů.
Bez zadané sítě CLI vybere skutečný 8/16px krok podle uložené síly. `baseline`
na konci příkazu použije detekci a směrové váhy před poslední úpravou.

`compare_contour_debug.py BEFORE_PREFIX AFTER_PREFIX --output DIR [--box X0 Y0
X1 Y1] [--scale 2]` vyžaduje shodné nativní pixely a síť. Vytvoří přesné
porovnání masek bez dilatace či normalizace a uloží počty přidaných/ztracených
pixelů s vahou >=8/255. CSV obsahuje důvody: analysis 1=alfa, 2=konstanta,
3=rovina/slabá křivost, 4=rozsah, 5=málo vzorků jedné strany, 6=energie,
7=chybí směr; localization 1=slabý barevný model, 2=normála, 3=rozsah fáze,
4=málo jednoznačných průchodů, 5=sklon, 6=rozptyl poloh. Nula znamená
vyhodnocený model, ne automaticky vysokou důvěru. Aktuální příklady:
`diagnostics/R460-contour-debug/debug-head` a `debug-whole`.

`compare_decode.py --references samples/references` navíc spočítá SSIMULACRA2
a RGB/edge-band chybu proti původnímu PNG. Enkodér se nespouští;
oba dekodéry dostávají identické uložené pakety. Přepínač je nepovinný.

`audit_cleanup_controls.py --exe EXE --input INPUT.fajk --output DIR` vytváří
diagnostické kopie se samostatně vypnutými ovladači a správným CRC. Maska je
rozdíl zaokrouhlených výsledných pixelů. `no-contours` izoluje uložené základní
konturové modely, nikoli všechny součásti AA/Skirt. Černý pixel této masky
proto sám nedokazuje, že nebyla detekována hrana. Měřte skutečné geometrické
modely výše. Výsledky aktuální iterace jsou v `diagnostics/R460-tolerance`.

## R462: kontrola alokace

`fajk_cli inspect-allocation INPUT PREFIX TARGET_BYTES FLAT_ALLOCATION_0_100`
vytvoří skutečný encode/decode, .fajk, PNG, mapy způsobilosti/penalizace/bitů,
dvojici mapa+výsledek a CSV každé buňky. Použijte stejný target pro 0/50/100.
50 musí obnovit základní rozdělení; jiné hodnoty mění skutečné kvantizační váhy.
Barevná mapa je jiná veličina než bity. Mapa způsobilosti není přesnou maskou
hran; šedé buňky obsahují více zdrojových klasifikačních buněk.

`verify_allocation.py --input "dev/samples/Snímek obrazovky (23006).png"
--output OUTPUT --cli dev/build-r4/fajk_cli.exe
--baseline-cli release/R461/app/fajk_cli.exe --metric dev/build-r4/fajk_metric.exe`
prověří stejné snímky při 30/80/150 kB, 0/50/100 a 25/75 u 80 kB, přesný
R461 baseline, monotónnost způsobilosti, limity velikosti a alfa/dimenze.
Vytvoří skutečné mapy, výřezy a JSON s orientačními náklady konkrétních ROI.
ROI souřadnice jsou určeny pro přiložený 23006 v 2560×1440. Geometrie mapy
se čte ze skutečného enkódu, nepředpokládá pevný poměr pracovního zmenšení.

## R465: bilance úspory ploch

`fajk_cli inspect-allocation INPUT PREFIX TARGET_BYTES FLAT_ALLOCATION_0_100 compare0`
navíc vytvoří `PREFIX-savings.txt/json`: referenční encode při úspoře 0 %,
stejný rozpočet i ostatní volby. Srovnává stejnou bílou masku aktuálního obrazu,
umí rozdílné pracovní rozlišení a okrajové buňky. Detailové bity jsou prostorově
rozprostřené podle podpory transformace; nejde o samostatně uložená data ploch.
Bilance zahrnuje i zbytek obrazu a velikost celého kontejneru, takže úspora v ROI
se nezamění za přírůstek v jediném jiném ROI. Na každý 30kB obraz nelze slíbit 2 kB.

`review_flat_allocation.py --compare-zero` přidá tuto bilanci do `results.json`.
Nástroj nově obsahuje i pevné výřezy auta/stěn snímku 22950.

`compare_aq.py --exe CLI --output DIR` porovná AQ vypnutou / 50 % / 100 %
na 30 a 80 kB při totožném vyhlazování. Uloží skutečné PNG a porovnání s originálem,
velikost celého souboru, čas, SS2 a pevné ROI. Jde primárně o vizuální kontrolu;
pořadí off/on a souběžná zátěž nejsou izolovaný rychlostní benchmark.

Finální R465: `compare_allocation_maps.py --exe CLI --output DIR` vytvoří
srovnání AQ vypnutá / síla úspory 0 / 50 / 100 %, vždy s jednou společnou
škálou skutečných bitů. `--vary-contrast --flat-savings 50` porovná místo toho
kontrast 0 / 50 / 100 %. Nahoře je skutečné rozhodnutí RDO, uprostřed bity,
dole obraz; JSON obsahuje ROI bity. `--reuse` pouze sestaví již hotové výstupy.

`inspect-allocation INPUT PREFIX TARGET CONTRAST compare0 STRENGTH AQ_ENABLED`
exportuje také `-routing.png`, `-tiles.csv` s místním krokem a `-allocation.csv`
s faktory kroku Y/C1/C2. Při nenulové síle je reference stejný kontrast / síla 0;
jinak kontrast 0. `measure` má poslední nepovinné parametry `AQ_ENABLED STRENGTH`.
`compare_aq.py --flat-savings 50` ukládá tuto hodnotu i do výsledků.

R470: `inspect-allocation` přijímá ještě poslední `THRESHOLD_0_100` za AQ_ENABLED;
`measure` poslední argument `THRESHOLD_0_100` za sílou úspory. Enkódovací CLI má
`--flat-threshold 0-100`. Výchozí je 0. `verify_sdk_pixels.py --dll DLL --packets
DIR... --output REPORT.json` ověří přesné RGBA proti PNG a neporušené guardy
při jednom a automatickém počtu vláken. `review_car_contour.py` vytvoří párové
měření a skutečné výřezy přibližného spodního obrysu auta; všechny panely jsou
pro lepší viditelnost tmavé kontury zobrazené stejně 3× světlejší.

## R470: srovnání FAJK / AVIF / JPEG XL

`compare_image_codecs.py --input SOURCE --output DIR --cli dev/build-r4/fajk_cli.exe
--jxl dev/build-r4/fajk_jxl_compare.dll --preset SETTINGS.ini --threads 10 --repeats 8`
porovnává celé soubory 30/80/150 kB. FAJK má target zmenšený o 168 B kontejneru.
AVIF AOM speed 6 / 4:2:0, JXL effort 7 / XYB jsou explicitní, nikoli shodně
náročné presety. Když AVIF quality 0 v plném rozlišení limit nesplní, uloží
nativní minimum a zvolí největší z pevných pracovních měřítek .875/.75/.625/.5,
které se vejde. Předzmenšení a následný Lanczos se započítávají do času.
JXL dovolí interní resampling 2/4/8 jen pokud běžný endpoint distance 25 nestačí.

Hledání parametrů AVIF/JXL je samostatně; finální encode/decode se měří z paměti
bez PNG I/O a startu procesu. FAJK encode zahrnuje své vlastní hledání velikosti.
FAJK decode zapisuje do připraveného RGBA bufferu, konkurenti zahrnují alokace
API a kopie. Časy proto nejsou čisté srovnání pouze entropy jader.
`measure` má nový poslední nepovinný argument `RECONSTRUCTION_INI`, aby se do
měření promítly přesně stejné skryté volby AA/ochrany textu jako v aplikaci.

`fajk_jxl_compare` je samostatná vývojová C API DLL nad místní libjxl 0.11.1.
Sestaví se s `FAJK_REFERENCE_METRIC=ON`. Nepřidává JPEG XL závislost do aplikace.
`render_codec_comparison.py DIR` vytvoří lokální HTML pro synchronní zoom a pan,
stejné nativní výřezy a `timings.csv`; výsledky a parametry zůstávají v JSON.

## Skutečné video CPU / GPU

Sestavte `fajk_video_benchmark` spolu s CLI. `verify_gpu_video.py INPUT.mp4
--output DIR --frames 24 --rounds 2 --workers 10 --budgets 30000 80000 150000
--full` střídá CPU/GPU při stejném celkovém limitu vláken. Export používá
produkční `convert`, každý paket a časování kontroluje proti samostatnému
enkódování. Ověřuje také zrušení probíhajícího exportu, zachování existujícího
souboru a konzervativní cestu při omezení RAM. Kvalitu vzorků měří SSIMULACRA2.

`--full` enkóduje celý původní klip přes CLI a bez uložení všech RGBA snímků
do RAM dekóduje všechny snímky, ověřuje CPU/GPU shodu a náhodné přístupy.
Přímé volání: `fajk_video_benchmark --decode INPUT.fajkv PREFIX WORKERS cpu|gpu`.
Samostatný video benchmark: `fajk_video_benchmark INPUT.mp4 PREFIX FRAMES
WORKERS PACKET_BYTES cpu|gpu MAX_WIDTH`; nula šířky zachovává původní rozlišení.

Čas sekvenčního dekódování zahrnuje čtení paketů a jejich kontrolní součty,
veškeré GPU přenosy, čekání a výsledný RGBA obraz. Nezahrnuje kontrolní hash
RGBA, zápis PNG, porovnání backendů ani vykreslení uživatelského rozhraní.
Export celého klipu zahrnuje start procesu; krátké měření měří přímo `convert`.
Měření nespouštějte souběžně s jinými benchmarky nebo sestavováním.

Křížový video decode omezuje průměrnou odchylku na 0,001 bajtu, podíl
změněných kanálů na 0,2 % a podíl odchylek nad 16 na 0,001 %. Vzácné
větší odchylky mohou vzniknout z jednobajtového zaokrouhlení před diskrétním
konturovým rozhodnutím. Takové snímky se vždy exportují a oba dekódy se
porovnají se skutečným původním snímkem: SSIMULACRA2 se nesmí lišit o více
než 0,02 bodu. Maximum zůstává v CSV/JSON, neskrývá se průměrem.
Při --full se původní MP4 čte znovu až v neměřené validační části.

`verify_worker_scaling.py --output DIR --repeats 16` střídá limity
6/8/10/12 vláken na stejném obrázku, druhé kolo v opačném pořadí.
Kontroluje přesnou shodu paketů a pixelů v rámci stejného backendu.
Jeden obrázek není dostatečným podkladem ke změně automatického limitu;
více vláken nemusí zrychlit paměťově omezené fáze ani GPU odesílání.

`verify_exact_optimization.py --before OLD_CLI --after NEW_CLI --inputs
SOURCE.png ... --output DIR` střídá celé encode/decode při společných
rozpočtech a kontroluje přesné pakety i RGBA pro každý backend a opakování.
`compare_video_builds.py --before OLD_VIDEO_HELPER --after NEW_VIDEO_HELPER
--input SOURCE.mp4 --output DIR` porovnává produkční video export a sekvenční
decode, při čtyřech různě řazených kolech a zachování paketů i CRC snímků.
`compare_decode.py --backend gpu --require-exact-pixels` umožňuje samostatné
párové měření stejného paketu; výchozí backend zůstává CPU.

R490: `verify_exact_optimization.py` nadále vyžaduje přesné pakety a ve
výchozím režimu přesné pixely. Pouze výslovné `--max-pixel-delta 1
--max-mean-delta .00001` dovolí ohraničené GPU zaokrouhlení. Zaznamenává
maximum, průměr, procesní CPU čas i CPU cykly. CPU větev zůstává přesná.
`verify_rounding_quality.py --results RESULTS.json --metric fajk_metric.exe
--output QUALITY.json` pak ověří změněné případy proti původnímu zdroji;
povolený pokles SSIMULACRA2 je nejvýše 0,001 bodu. Zlepšení skóre není
regrese; oboustranná pixelová mez přesto zůstává přísná.

`verify_gpu_sdk.py` porovnává přesné CLI pixely se stejným backendem a
stejným rozpočtem vláken. CLI R490 přijímá `decode ... --workers N`.
To pokrývá i různé automatické volby AVX2/GPU postfiltrů při 1/2/10 vláknech,
padding stride, ochranné bajty a dva souběžné nezávislé handles.

`summarize_stage_profiles.py --profiles FILE-stages.csv ... --output REPORT.json`
sčítá opakované pokusy v každém snímku a poté počítá aritmetické průměry.
Nezapočítává vnořené pack/detail/cleanup intervaly podruhé. Volitelné prostředí
`FAJK_PROFILE_SPATIAL=1` při CLI measure přidá `encode.work.spatial.*`:
součty času pracovníků pro přípravu, volbu přesnosti, kvantizaci, RDO a coding.
Nejde o uplynulý čas snímku a nesmí se přičítat k encode_ms; jemné časování
má vlastní režii, normální měření je vypíná. `--reconstruction-ini FILE`
u verify_exact_optimization přenese stejné rekonstrukční nastavení do obou
porovnávaných CLI. Nulové krátké CPU časové tiky se agregují součtem před
dělením; CPU cykly a celkové časy mají geometrický průměr poměrů.

`verify_fast_modes.py --baseline QUALITY_CLI --cli NEW_CLI --output DIR
--ini SETTINGS --metric METRIC --inputs PNG ... --budgets 30000 80000 150000
--backends cpu gpu` střídá baseline, explicitní More quality a Fast. Vyžaduje
přesnou shodu More quality s baseline, deterministické pakety v každém režimu,
správné rozměry, limit bajtů a GPU práci bez chybového fallbacku. Časy jsou
aritmetické průměry po warmupu; SSIMULACRA2 běží až po všech měřeních.
Fast se neověřuje proti pixelové shodě s More quality: může měnit kvantování.

`verify_fast_video.py --input MP4 --build BUILD --baseline QUALITY_VIDEO_DIR
--output DIR --metric METRIC [--full]` měří oba režimy na CPU/GPU a ponechá
přísné nativní kontroly nezávislého/pipeline kódování, časových značek,
cross-decode, zrušení, RAM limitu a seeku. Parametr `--full` exportuje celé
Fast video a dekóduje každý snímek oběma backendy. Nativní video helper nově
přijímá devátý nepovinný argument `fast|quality`; bez něj zůstává More quality.

`fajk_temporal_benchmark PHOTO VIDEO OUTPUT` střídá CPU/GPU a intra/temporal
ve dvou kolech: 24 identických snímků, 24 lokálních změn, 24 skutečných MP4
snímků. Časy zahrnují kontejner, přenosy, reference i kopie; PNG a kontroly
jsou mimo čas. Každý delta pixel musí zachovat bodovou mez chyby keyframu.

`write_temporal_test_avi.py PHOTO OUTPUT.avi` vytvoří 32 přesných RGB snímků
pro test produkční pipeline, bez potřeby FFmpeg. `verify_temporal_pipeline.py
--cli CLI --baseline ARCHIVED_CLI --screen AVI --motion MP4 --output DIR`
měří celé procesy a kontroluje CRC, indexy, adaptivní interval a přesnou
shodu nezměněného intra režimu s vydanou .1. `fajk_temporal_tests AVI` navíc
testuje zrušení se zachováním cíle, sampling, 256 MiB nastavení a výstup
jediného snímku. Bez AVI běží pravidelné CTest testy i mutace paketů.

`verify_temporal_sdk.py --library DLL --cli CLI --input TEMPORAL.fajkv
--output DIR` kontroluje náhodný seek, změny backendu a postfiltrů v jednom
handle, dvě souběžná handles a guard/padding při 1/2/10 vláknech.

`fajk_similar_benchmark PHOTO VIDEO OUTPUT [FOCUSED_TOLERANCE]` porovnává
nezávislé snímky, přesné rozdíly a tolerance 3/6/12 při změně jasu, místním
pohybu a skutečném MP4. Dvě kola střídají pořadí; volitelný čtvrtý argument
omezí běh na herní klip a jednu toleranci. Ověřuje přesné zdrojové pixely
číselně opravených bloků a bodovou mez zdrojové změny u převzatých bloků.
Časy zahrnují porovnání, případnou rekonstrukci reference, neúspěšné pokusy,
kompresi i kontejner. PNG a pixelové kontroly jsou mimo časování.
Výchozí AQ i textová ochrana jsou v R500 vypnuté. Při porovnání se starými
binárkami nastavte obě volby výslovně a shodně; stará výchozí nastavení se liší.

`fajk_final_filter_benchmark PHOTO.png OUTPUT.csv` měří samostatné finální
ostření 0/50/100 při 1/2/10 vláknech na CPU/GPU, 16 opakování po warmupu.
Započítává snapshot, upload, dispatch, readback i čekání; reset vstupu je mimo
čas. GPU volba může použít AVX2, pokud je s více CPU vlákny rychlejší.

`verify_final_filters.py --baseline-cli OLD_CLI --baseline-dll OLD_DLL
--cli NEW_CLI --dll NEW_DLL --output DIR --sources PNG ... --metric METRIC`
ověří přesné zakódované pakety při 30/80/150 kB, kontrolní nulové nastavení,
účinnost a monotonicitu ostření, ochranné bajty a finální SSIMULACRA2.
Celý decode staré/nové verze se střídá v osmi opakováních po warmupu.
Percepční metriky a PNG export běží mimo časování; vyšší síla ostření nemusí
znamenat vyšší skóre. Pro skutečný regresní závěr rozlišujte změnu obrazu
způsobenou aktivovanými konturami od režie stejné operace.

`verify_padded_decode.py --before OLD_DLL --after NEW_DLL --inputs VIDEO ...
--output DIR [--repeats 24]` střídá skutečné SDK dekodéry při CPU/GPU,
2/10 vláknech a výplni řádků 0/28 B. Zaznamená všechny časové vzorky;
pixelové SHA, časování snímku, výplň a guard bajty se kontrolují mimo čas.
Souvislý výstup je kontrolní případ; recyklace padded scratch nemá zrychlovat
tuto cestu. Nové `fajk_sdk_tests` ověřují C ABI nad skutečnou DLL v Release
i ASAN a jsou součástí běžných Full testů.

R520: `verify_motion_temporal.py --cli CLI --library DLL --video VIDEO
--hold HOLD.avi --output DIR --frames 240 --repeats 2` ověřuje skutečné
exporty Max proti intra (30/80 kB, CPU/GPU), opakovanou shodu paketů,
CRC/index, limity bajtů a řetězců, pětisekundové PTS i dlouhé statické držení.
Až po měření exportů ověří každý snímek přes SDK s ochrannými bajty a
náhodné čtení z nového handle. Max je ztrátový: netvrdí rovnost s intra.
`fajk_motion_quality_probe VIDEO INTRA MAX OUTPUT COUNT BACKEND [SNAPSHOTS=1]
[MOTION_FLICKER=0]` měří chybu proti původnímu videu a časové změny chyby.
Volitelná pohybová metrika zarovná předchozí zdroj i oba dekódované obrazy
stejným polem z původních snímků; přispívají pouze zdrojové shody do 3 RGB.
Nejde o úplnou perceptuální záruku ani metriku odkrytých oblastí. Pro obrázky
použijte také `fajk_metric` (SSIMULACRA2) a vizuální kontrolu.

`package_release.py --package release/R520 --build dev/build-r4 --archive
release/R520-20261025.zip --validation RESULTS.json` zkontroluje shodu EXE/DLL/LIB
s buildem a stejné verze z Windows resources. Vytvoří BUILD.json s hashi,
vynechá osobní nastavení a ověří CRC i SHA256 všech souborů uvnitř ZIP.
Existující archiv nepřepisuje; výsledný soubor zveřejní atomickým přejmenováním.

Optimalizace po R520: `compare_motion_exports.py --baseline-cli OLD_CLI
--candidate-cli NEW_CLI --video VIDEO --output DIR [--repeats 3]
[--budgets 80000 30000] [--backends gpu cpu] [--require-identical]` střídá
skutečné exporty obou verzí, kontroluje opakovanou determinističnost,
CRC/indexy, rozpočty a hloubku referencí. Zaznamenává wall time i součet práce
CPU. Volba `--require-identical` je vhodná pro změny bez změny paketů;
kompaktnější bezztrátový zápis metadat může změnit soubor bez změny obrazu.

`compare_motion_decoders.py --before OLD_DLL --after NEW_DLL --before-video
OLD.fajkv --after-video NEW.fajkv --output RESULTS.json [--repeats 2]`
porovná každý snímek přes skutečné SDK na CPU i GPU, ochranné bajty,
časové údaje, opakovanou determinističnost a seek z nových handles.
Vyžaduje přesné shodné pixely pro daný backend. Hashování je mimo měření.
Časujte bez souběžných buildů, exportů či quality probes.
Pro úmyslně ztrátové změny přijímá `compare_motion_decoders.py` přepínač
`--allow-pixel-differences`; přesné shody pak vyžaduje zvlášť mezi opakovaným
a náhodným dekódováním každého souboru. Časové údaje obou exportů musí stále
souhlasit. V takovém režimu výkonový test nehodnotí kvalitu; ta potřebuje
samostatný source quality probe. Sekvenční část dekóduje celý soubor pro jeden
handle před druhým, aby dva soubory stejné DLL navzájem nezahazovaly TLS cache.

GPU testy mají nezávislý oracle pro spojené warpy, stacionární volbu 8x8,
reziduum a dopřednou transformaci. Kontrolují i objem přenosů, liché
rozměry, barevné posuny, masky přesných shod a nepozměněný CPU fallback.
Motion testy porovnávají kompaktní metadata s původním polem a hlídají,
že menší metadata sama nezmění původní rozhodnutí o přijetí predikce.
Řízený test jednotné změny osvětlení vyžaduje přesné rekonstruované pixely;
jeho procento úspory se nesmí vydávat za úsporu celého herního videa.

Příprava pohybu v Max nyní hledá odpovídající oblasti v původním svědkovi
reference a zbytkovou chybu stále počítá vůči skutečné uzavřené rekonstrukci.
Jde o změnu obrazu: porovnávejte celý klip pomocí motion_quality_probe a
perceptuální kontrolní snímky, nevyžadujte rovnost s předchozím enkodérem.
Tato předchozí úprava používala nezměněný formát 137/14. Nová rekonstrukční
filtrace Max níže používá variantu rezidua 15 a vyžaduje aktuální dekodér;
kompatibilita se starými aplikacemi není jejím cílem.

Private CoefficientEncodeCapture předává vybrané integer koeficienty keyframu
jeho kanonické rekonstrukci. Použije se pouze při shodě kompletního native
paketu, rozměrů a vybraného kvantizačního kroku; jinak se normálně dekóduje.
Nepodporovaná flat metadata capture vypínají. Test motion_temporal porovnává
tuto cestu s nezávislým dekodérem pro CPU/GPU, Fast/More quality, AQ off/on,
změnu identity a různé rozpočty. Případné filtrování je stejné jako původně.

UI udržuje zvlášť generaci požadavků a revizi obsahu. Seek, play a filtry
zahodí staré požadavky, ale zachovají čtečku/index a reference. Nově otevřený
či přepsaný obsah zvyšuje revizi i při stejné cestě. Smoke log
decode-cache-smoke.txt počítá skutečná otevření čteček pouze při smoke běhu.

`verify_compression_candidate.py --cli FINAL_CLI --library FINAL_DLL
--old-library OLD_DLL --video INPUT --output DIRECTORY --measurements
GPU_RESULTS.json CPU_RESULTS.json [--hold long-hold.avi]` znovu exportuje
všechna měřená nastavení finálním sestavením a požaduje stejné SHA256 celých
souborů. Následně kontroluje každý snímek, ochranné bajty, časové údaje,
náhodné seeky a pixelově přesnou kompatibilitu starého/nového SDK na CPU/GPU.
Volitelný hold je konkrétní fixture z Temporal-20261024: 420 snímků,
jediná změna po 12 sekundách, keyframy pouze na indexech 0 a 360.

`evaluate_motion_strength.py --cli CLI --video INPUT --intra-reference
INTRA_RESULTS.json --output DIR --case gpu:80000:6 --case gpu:80000:12`
exportuje a strukturálně ověří jednotlivé síly Max. Úspora je vztažená
k uloženému intra exportu stejného backendu/rozpočtu/počtu snímků. Nástroj
záměrně nevyvozuje zrychlení z běhů zatíženého počítače.

`verify_motion_strength_quality.py --probe PROBE --metric METRIC --video INPUT
--exports STRENGTH_RESULTS.json --baseline-directory OLD_EXPORTS --output DIR
[--case gpu:80000:6] [--library NEW_DLL --old-library OLD_DLL]` ověří kvalitu
každého snímku vůči originálu, RMS změny chyby ve stacionárních oblastech
i po zarovnání pohybu, náhodné seeky a šest explicitních snímků SSIMULACRA2.
Baseline directory obsahuje gpu-80000.fajkv, gpu-30000.fajkv, cpu-80000.fajkv.
Volitelné SDK porovnání vyžaduje stejné pixely starého/nového dekodéru nad
stejným novým souborem. Nezaměňujte šest perceptuálních snímků za hodnocení
celého videa, ani nižší flicker RMS za záruku nepřítomnosti ghostingu.

Max dovoluje při přeplnění datového limitu jeden hrubší pokus nad stejným
reziduem. Ukládá transformaci před kvantizací a koeficienty rekonstruuje až
po přijetí kroku. Hledání pohybu, GPU přípravu ani přenosy neopakuje. Velká
vzorkovaná chyba prediktoru hrubší pokus vypíná; při neúspěchu následuje
keyframe. MaximumStep je interní řízení enkodéru, vybraný krok už je součástí
stávajícího formátu 134/14. Síla Max nevyjadřuje garantovanou RGB chybu.
Základní křivka v rozsahu síly 0–6 zůstává jemná. Dodatečná úspora se plynule
přidává v rozsahu 7–12; vyšší agresivní nastavení není automaticky zapnuté
ani při malém datovém limitu. U 30 kB způsobovala strmější křivka i na síle 6
nepřiměřenou ztrátu detailů, proto byla pro běžné nastavení odmítnuta.

Aktuální Max s čistšími barvami používá 134/15: potlačí slabé korekce
v nejjemnějších dvou barevných úrovních a nejjemnějším diagonálním jasu,
poté uvnitř
referenční smyčky prostorově vyhladí chroma podle jasových hran. Jas filtruje
podstatně slaběji. Přesné source-skip bloky se nefiltrovaně zachovají, takže
se statické části opakovaným filtrováním nerozmazávají. Enkodér i dekodér
provádějí tutéž celočíselnou rekonstrukci; náhodný seek nesmí měnit výsledek.
U větších obrazů GPU backend provede celočíselný referenční filtr na GPU;
přenese pouze zabalené RGBA a malou mapu zachovaných oblastí, bez float rovin.
GPU musí vrátit přesně stejné bajty jako CPU, včetně záporného zaokrouhlení,
okrajů, alfy a přesných skip bloků. Malé obrazy a neúspěšné GPU volání použijí
CPU; neúspěšný pokus nesmí změnit hostitelský obraz.
Nejde o směšování různých snímků; hrubší kvantizace stále může ztrácet detaily.
Pod krokem 4 je prostorový filtr identita, mezi kroky 4–15 jeho rozsah klesá;
nejjemnější reziduum nemá povinnou minimální sílu rozmazání.
Plynulý firm práh ponechá silné koeficienty beze změny; tvrdé vyřazování čtyř
barevných úrovní bylo odmítnuto, protože mazalo i skutečné barevné struktury.
Do rezidua se přenáší jen polovina omezené společné jasové chyby reference;
nezávislá chyba R/G/B se dále nepřebírá. Úplné přebírání jasové chyby by
integrovalo novou kvantizační chybu v každém dalším snímku. GPU i CPU používají
stejnou celočíselnou formuli. Pohybový CPU fallback nyní respektuje vypnutí
odhadu osvětlení a vyžaduje stejnou robustní podporu společného posunu jako GPU,
aby lokální změna objektu nezabarvila celý obraz. Starší diagnostické režimy
si zachovávají vlastní původní pravidla.

`compare_flat_noise.py --before OLD_SNAPSHOTS --after NEW_SNAPSHOTS --output
RESULTS.json [--frames 1 12 23 47 119 239]` měří jemnou jasovou/barevnou
chybu v plochách vybraných z originálu společnou maskou RGB rozdílů do 4.
Chroma RMSE zachycuje i širší barevné odchylky. Tento test snímků nenahrazuje
celoklipovou kontrolu blikání ani vizuální kontrolu postav v pohybu.
`--before-suffix intra` porovná přímo baseline obrazy uložené stejným probe;
v tomto názvu baseline nemusí znamenat samostatně kódované snímky.

motion_quality_probe dekóduje baseline na odděleném stálém vlákně. Dva
nezávislé soubory tak nevyhazují thread-local referenční cache jeden druhému.
Požadavky zůstávají sekvenční; staré interleaved časy probe byly zkreslené
opakovaným dekódováním celých referenčních řetězců. Kvalitativní metriky
zůstávají stejné; pro výkon používejte samostatné exportní benchmarky.
