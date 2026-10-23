"""Verify and freeze R500.1 without overwriting the R500.0 release evidence."""
import csv, hashlib, json, shutil, statistics, zipfile
from pathlib import Path

folder=Path(__file__).resolve().parent;root=folder.parents[2]
old=root/'dev/diagnostics/Temporal-20261023'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def read_log(p):
    b=p.read_bytes();return b.decode('utf-16') if b.startswith(b'\xff\xfe') else b.decode('utf-8-sig')
assert '100% tests passed, 0 tests failed out of 8' in read_log(folder/'release.log')
assert '100% tests passed, 0 tests failed out of 8' in read_log(folder/'asan.log')
for build in ['build-r4','build-asan']:assert 'pipeline:' in read_log(folder/f'pipeline-{build}.log')
assert all('72 SDK random decodes' in read_log(folder/(name+'.log')) for name in ['sdk-exact','sdk-multiple-keys','sdk-corrections'])
ui=json.loads((folder/'ui-results.json').read_text(encoding='utf-8-sig'));assert ui['exit_code']==0 and ui['pass_records']>=46 and ui['unified_backend']
assert json.loads((folder/'cli/results.json').read_text())['complete']
padded=json.loads((folder/'padded/results.json').read_text());assert padded['complete'] and len(padded['cases'])==16
assert all(r['identical_pixels'] for r in padded['cases'])
before=list(csv.DictReader((old/'r500-similar/timings.csv').open()))
after=list(csv.DictReader((old/'vector-check/timings.csv').open()));assert len(before)==len(after)==30
assert all(int(r['gpu_fallbacks'])==0 for r in after)
payloads=sorted((old/'vector-check').glob('*.fajkv'));assert len(payloads)==15
for p in payloads:assert p.read_bytes()==(old/'r500-similar'/p.name).read_bytes(),p.name
assert sha(root/'release/R500-20261023.zip')=='6b5e0f95475a6061006bd95e0ad27fa8d419f9dbac139e04cc9d7c58d1cd411a'
assert sha(root/'release/R490/settings.ini')=='4e93e5c3da18368f75b3d29a484adbcc2c50e985521dac6dc0556154c88d1007'
assert sha(root/'release/R500/settings.ini')=='09ed037f5d72dc1d1255d2e42d05542a7e556277b740bda2eb986327985e2f9b'
speed=[]
for scenario in ['lighting','local','motion']:
    entry=dict(scenario=scenario,tolerance=3,frames=24)
    for label,rows in [('before',before),('after',after)]:
        group=[r for r in rows if r['scenario']==scenario and r['tolerance']=='3']
        entry[label]={k:statistics.mean(float(r[k]) for r in group) for k in ['encode_ms','temporal_ms','bytes']}
    speed.append(entry)
summary=dict(version='0.11.500.1',encode=speed,padded=padded['cases'],exact_video_files=15,scalar_oracle_cases=3888,native_sdk_decodes=648,external_sdk_decodes=216,ui=ui)
(folder/'results.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
text='''# R500.1 — přesné porovnávání a opakované použití SDK bufferů

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
'''
for r in speed:
    b,a=r['before'],r['after'];text+=f"\n- {r['scenario']}, 24 snímků: encode {b['encode_ms']:.3f} → {a['encode_ms']:.3f} ms, úspora {100*(1-a['encode_ms']/b['encode_ms']):.1f} %. Porovnání a korekční pokusy {b['temporal_ms']:.3f} → {a['temporal_ms']:.3f} ms. Velikost shodná: {int(a['bytes']):,} B."
text+='''

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
'''
for r in padded['cases']:
    if r['padding']:
        text+=f"\n- {Path(r['input']).name}, {'GPU' if r['backend'] else 'CPU'}, {r['workers']} vláken: {r['before_ms']:.3f} → {r['after_ms']:.3f} ms ({100*(1-r['after_ms']/r['before_ms']):.1f} % méně)."
text+='''

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
'''
(folder/'RESULTS.md').write_text(text,encoding='utf-8')

package=root/'release/R500';checkpoint=folder/'checkpoint';archive_path=root/'release/R500-p1-20261023.zip'
assert not checkpoint.exists() and not archive_path.exists(),'Frozen patch already exists'
for name in ['FajkStudio.exe','fajk_cli.exe']:assert sha(package/'app'/name)==sha(root/'dev/build-r4'/name)
for name in ['fajk_decode.dll','fajk_decode.lib']:assert sha(package/'app/SDK'/name)==sha(root/'dev/build-r4'/name)==sha(root/'dev/SDK'/name)
assert (package/'app/SDK/decode.h').read_bytes()==(root/'dev/include/fajk/decode.h').read_bytes()==(root/'dev/SDK/decode.h').read_bytes()
assert (package/'README.txt').read_bytes()==(root/'dev/release-files/README.txt').read_bytes()
checkpoint.mkdir();files=[]
for name in ['src','include','tests','tools']:
    files.extend(p for p in (root/'dev'/name).rglob('*') if p.is_file() and p.suffix in ['.cpp','.c','.h','.hpp','.inl','.rc','.manifest','.xaml','.py','.md','.ps1'])
files.extend((root/'dev').glob('*.ps1'))
files.extend(root/p for p in ['dev/CMakeLists.txt','README.md','Spustit Fajk Studio.cmd','dev/SDK/README.md','dev/docs/CURRENT.md','dev/release-files/README.txt'])
files.extend(folder/p for p in ['validate.ps1','finalize.py','RESULTS.md','results.json'])
files=sorted(set(files))
with zipfile.ZipFile(checkpoint/'sources.zip','x',zipfile.ZIP_DEFLATED) as z:
    for p in files:z.write(p,p.relative_to(root).as_posix())
with zipfile.ZipFile(checkpoint/'sources.zip') as z:
    assert z.testzip() is None
    assert all(z.read(p.relative_to(root).as_posix())==p.read_bytes() for p in files)
for name in ['FajkStudio.exe','fajk_cli.exe','fajk_decode.dll','fajk_decode.lib','fajk_similar_benchmark.exe','fajk_temporal_benchmark.exe','fajk_sdk_tests.exe']:
    shutil.copy2(root/'dev/build-r4'/name,checkpoint/name)
shutil.copy2(old/'r500-similar/timings.csv',checkpoint/'before-encode.csv')
shutil.copy2(old/'vector-check/timings.csv',checkpoint/'after-encode.csv')
shutil.copy2(folder/'padded/results.json',checkpoint/'padded.json')
payloads=sorted(p for p in package.rglob('*') if p.is_file() and p.name!='settings.ini')
with zipfile.ZipFile(archive_path,'x',zipfile.ZIP_DEFLATED) as z:
    for p in payloads:z.write(p,p.relative_to(package.parent).as_posix())
with zipfile.ZipFile(archive_path) as z:
    assert z.testzip() is None
    assert all(z.read(p.relative_to(package.parent).as_posix())==p.read_bytes() for p in payloads)
manifest=dict(version='0.11.500.1',archive=str(archive_path),archive_sha256=sha(archive_path),archive_bytes=archive_path.stat().st_size,archive_entries=len(payloads),source_files=len(files),release_tests='8/8',asan_tests='8/8',native_sdk_decodes=648,external_sdk_decodes=216,scalar_cases=3888,exact_video_files=15,ui_pass_records=ui['pass_records'],files={p.name:sha(p) for p in checkpoint.iterdir() if p.is_file()})
(checkpoint/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
print(json.dumps(manifest,indent=2))
