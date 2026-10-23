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
