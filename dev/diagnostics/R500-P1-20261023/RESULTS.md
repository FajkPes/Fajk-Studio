# R500.1 — přesné porovnávání a opakované použití SDK bufferů

Oprava 0.11.500.1, 23. 10. 2026. Windows x64, Ryzen 5 5600X, RTX 3070,
48 GB RAM. Výchozí AQ/text zůstávají vypnuté; nastavení R500 se nezměnilo.
Původní R500.0 ZIP i jeho zdrojový checkpoint zůstaly zachované.

## Co se změnilo

- Porovnávání podobných oblastí používá SSE2 nad čtyřmi RGBA pixely současně.
  Absolutní rozdíly jsou přesné celé hodnoty; alfa má nulovou toleranci.
  Žádné zprůměrování, změna prahu ani další přenos na GPU. Úzké okraje mají
  skalární konec bez čtení za řádek. Paralelní dělení práce po řádcích bloků
  i původní limity vláken zůstaly zachované.
- SDK při výstupu s výplní řádků znovu používá jeden souvislý pomocný obraz
  do 64 MiB na handle. Odpadá opakovaná alokace a nulování celého obrazu.
  Větší obrazy používají dočasný buffer, fajk_close cache uvolní.
  Souvislý výstup se stále dekóduje přímo do bufferu volajícího.
- Full testy nově zahrnují skutečnou SDK DLL a její C ABI také pod ASAN.
  Předchozí nativní ASAN testy SDK API nepokrývaly; externí Python SDK
  kontroly jej ověřovaly jen v obyčejném Release. Tato mezera je doplněná.

## Encode, stejné videosoubory

Fast, GPU, 10 CPU vláken, 80 000 B na paket, tolerance 3, držení 10 snímků.
Průměry dvou kol po warmupu. Časy zahrnují porovnání, reference, pokusy,
encode a kontejner. Příprava zdrojů, PNG a pixelové kontroly jsou mimo čas.
Nejde o celkový čas otevření MP4/GUI. Měření sady .1 proběhlo po sadě .0;
mezi běhy je běžný systémový šum. Uvádíme celé vzorky pro posouzení.

- lighting, 24 snímků: encode 121.409 → 89.399 ms, úspora 26.4 %. Porovnání a korekční pokusy 48.339 → 16.711 ms. Velikost shodná: 237,476 B.
- local, 24 snímků: encode 172.233 → 139.547 ms, úspora 19.0 %. Porovnání a korekční pokusy 99.405 → 66.616 ms. Velikost shodná: 541,872 B.
- motion, 24 snímků: encode 521.560 → 507.517 ms, úspora 2.7 %. Porovnání a korekční pokusy 33.157 → 20.210 ms. Velikost shodná: 1,886,126 B.

Všech 15 úplných videosouborů (3 scény × intra/tolerance 0/3/6/12) je
bajtově shodných s R500.0. Tím se zachovává i oprava ducha z předchozí
verze. Herní klip nadále využije celé nezávislé snímky, bez temporal úspory;
tato oprava zkracuje jeho rozhodování, nepřidává pohybovou kompenzaci.
Přesný temporal režim s tolerancí 0 a původní intra encode nepoužívají
novou podobnostní smyčku, proto u nich neslibujeme stejné zrychlení.

## SDK decode s výplní řádků

24 střídavých vzorků staré/nové DLL po warmupu. Měří se jen SDK volání,
včetně kopie do řádků volajícího; SHA, metadata a guardy se kontrolují mimo
čas. Níže jsou průměrné ms na snímek při stride = width*4 + 28.
Souvislý výstup je v JSON jako kontrolní případ a nemá plánované zrychlení.

- local-t3.fajkv, CPU, 2 vláken: 8.589 → 6.316 ms (26.5 % méně).
- local-t3.fajkv, GPU, 2 vláken: 6.545 → 4.027 ms (38.5 % méně).
- local-t3.fajkv, CPU, 10 vláken: 5.767 → 3.351 ms (41.9 % méně).
- local-t3.fajkv, GPU, 10 vláken: 5.944 → 3.699 ms (37.8 % méně).
- motion-gpu-intra.fajkv, CPU, 2 vláken: 40.943 → 38.866 ms (5.1 % méně).
- motion-gpu-intra.fajkv, GPU, 2 vláken: 21.680 → 19.122 ms (11.8 % méně).
- motion-gpu-intra.fajkv, CPU, 10 vláken: 16.499 → 14.200 ms (13.9 % méně).
- motion-gpu-intra.fajkv, GPU, 10 vláken: 14.659 → 12.334 ms (15.9 % méně).

Tato úspora se týká padded SDK výstupu. Běžné Studio používá souvislé
recyklované buffery, takže se na něj tento dílčí decode přínos nevztahuje.
SDK stále kopíruje dokončený obraz do řádků volajícího; filtry potřebují
souvislou plochu. Nová cache má výslovný paměťový limit a vlastnictví handle.

## Ověření

- Release 8/8 a ASAN 8/8, nyní včetně samostatného testu SDK DLL.
- 3 888 případů vektorového porovnání proti nezávislému skalárnímu výpočtu:
  přesné prahy, alfa, šířky 1/2/3/4/5/7/15/16/17, úzké okraje,
  nezarovnané adresy a výplň řádků. ASAN kontroluje i hranice paměti.
- 648 nativních SDK dekódování v každé sestavě: průhledné přesné rozdíly,
  číselné korekce, více keyframů, náhodný seek, 1/2/10 vláken, CPU/GPU,
  stride s výplní 0/1/28, postfiltry, zotavení po chybě a guard bajty.
- 216 externích SDK/CLI kontrol na třech videosouborech, včetně dvou
  souběžných handles a změn backendu/postfiltrů v jednom handle.
- Produkční video pipeline v Release i ASAN: tolerance na CPU/GPU,
  zrušení zachová cíl, 256 MiB nastavení, sampling a jediný snímek.
- 16 párových SDK měření se shodnými pixely, metadaty a neporušenou výplní.
- 46 UI kontrol: společné CPU/GPU, Fast, persistence, tolerance/hold,
  finální ostření bez nového encode, export a render. CLI aliasy a AQ
  číselná síla / hlavní přepínač prošly samostatnou kontrolou.
- Při kontrole dekódovací reference a její invalidace nebyla nalezena další
  potvrzená chyba. Jde o rozsah uvedených kontrol, nikoli důkaz bezchybnosti
  každého vstupu. Další velké zrychlení herní temporal komprese stále vyžaduje
  kvalitní pohybovou predikci.

Podklady: results.json, padded/results.json, asan.log, release.log,
sdk-*.log, pipeline-*.log, ui-results.json a checkpoint/manifest.json.
Původní encode vzorky: ../Temporal-20261023/r500-similar/timings.csv;
vektorové vzorky: ../Temporal-20261023/vector-check/timings.csv.
Zmrazené kopie obou CSV jsou v checkpoint. Reprodukce: validate.ps1 po
sestavení Release. Původní R500.0 a jeho omezení popisuje
[předchozí protokol](../Temporal-20261023/RESULTS.md).
