Fajk Studio R600.4 — 0.11.600.4, Windows x64

Nový Režim komprese: Max komprese · Fast. Zachovává rychlé hledání Fast
bez drahého kvantizačního RDO a synthesis pilotu. Před kvantizací mírně
koriguje silné barevné detaily; malé koeficienty se nezesilují. Při vyšším
rozpočtu používá o něco vyšší pracovní rozlišení a model koeficientů 33,
který má kontexty moderního modelu bez mapy adaptivní přesnosti a bez
dodatečných pokusů o překódování každého pásma. Při malém rozpočtu ponechá
původní kompaktní kódování. AQ se tím nezapíná a volba barevných složek
se nemění. Fast i More quality zůstávají dostupné.

Volba je samostatná vůči Mezisnímkové kompresi / Max kompresi s pohybem.
Platí pro fotografie i video. CLI: --fast-max; s pohybem přidejte
--temporal-max, pro přesné mezisnímkové překódování --temporal-coefficients.
Model 33 vyžaduje dodaný dekodér R600.4. Starší soubory se čtou beze změny.
Vyšší pracovní rozlišení může mírně prodražit decode; kvalita a čas závisejí
na obsahu i rozpočtu. Režim není zárukou poloviční chyby ani menšího souboru
než Fast při stejné nastavené kvalitě. Srovnání používá stejný limit velikosti.

R600.3 zobrazuje nastavenou i výslednou sílu Skirt/deringu. Automatický režim
ponechává plnou nastavenou sílu do kvality 10, mezi 10 a 50 ji lineárně snižuje
a od 50 ji vypíná. Nadále zohledňuje skutečnou hustotu dat. Vypnutí volby
Automaticky snižovat podle kvality zachová ruční sílu bez obou útlumů.
Posuvník se automatickým útlumem nepřepisuje. Před výpočtem se ukazuje odhad,
po výpočtu náhledu síla vypočtená ze skutečně uloženého paketu.

V Doplňkovém čištění u Kontur a tvarů je Tolerance tvarů a Minimální měřítko
tvarů, obě 25–200 %, výchozí 100 %. Hodnota 100 % zachovává předchozí
rozpoznávání. Vyšší tolerance dovolí větší odchylku barev/fitované geometrie;
vyšší minimální měřítko dává přednost větším tvarům. Měřítko je relativní
velikost analyzovaného okolí v pracovním obrazu, nikoli ostrý pixelový limit.
Obě hodnoty lze najednou obnovit tlačítkem na 100 %.
Ruční režim i vlastní nastavení tvarů se ukládají do souboru; pro jejich
přehrávání použijte dodaný dekodér R600.3. Původní soubory se čtou beze změny.

R600.2 přidává finální ostření do200 %, rychlejší CPU řádkové zpracování
s předpočítaným jasem a více vláken pro GPU kontury. Hodnota100 % zachovává
dosavadní maximální sílu;200 % zesílí jasové detaily v témže průchodu.

R600.2 upravuje ukazatel rychlosti přehrávače: hlavní údaj je naměřený
průměr aktivního času na hotový snímek a rychlost dekodéru ve snímcích/s.
Průměr posledních až32 dokončených snímků započítává souběh větví jednou
a vynechává čekání na plnou frontu. Latence konkrétní úlohy a převzetí
z fronty jsou uvedeny samostatně. Přeskok měření resetuje.
Opraveny jsou také názvy CPU/GPU v nabídce enkodéru.

R600.2 spojuje přesný GPU renderer základního čištění s konturami, zvětšením
a ostřením. Nativní GPU obraz může zůstat v omezené cache bez opakovaného
uploadu; při vyřazení funguje cesta z RAM. CPU stále připravuje vodicí data.
Max porovnává přesné alternativy z nedávné reference a původního keyframu.
Samostatný přesný režim zachovává kratší řetězce z nedávné reference.
Platí maximálně pět sekund změněného obrazu. CPU Max
sdílí jednu kanonickou rekonstrukci. Formát ani ABI se nemění.

Opravné vydání R590.1 chrání koeficientovou a pohybovou cache před uložením
upravené kopie paketu pod identitou jiného snímku. Max navíc kontroluje
lokální jasový šum a švy; barevné a časové kontroly zůstávají zachovány.
Starší soubory dekóduje stejně. Tato ochrana se projeví u nových exportů;
již uložené artefakty nelze touto změnou zpětně odstranit.

R590.1 zrychluje přesnou koeficientovou predikci a obnovu pásem pomocí AVX2.
Dekodér znovu používá pracovní pole v rámci svého paměťového rozpočtu;
pole vrací do zásoby až poslední vlastník. Dřívější vypůjčené reference
zůstávají neměnné. Procesory bez AVX2 používají skalární cestu.
Návrat k přesně stejnému zakódovanému klíčovému snímku po jiném obrazu
může použít osmibajtový odkaz. Limit obnovy změněného obrazu do pěti
sekund zůstává zachován. Výstupní pixely rychlostních úprav se nemění.

Spusťte Spustit Fajk Studio.cmd. Společná volba R600.2 CPU / R600.2 GPU platí
pro kompresi, náhled i přehrávání. Fast / More quality je samostatná volba.
AQ a ochrana textu jsou výchozí vypnuté a zůstávají nastavitelné.

R590.1 zachovává paralelní přehrávání komprimovaného GPU náhledu: dvě vlastní
větve dekodéru a omezené předdekódování pěti snímků. Celkový limit CPU
vláken zůstává společný, nepřiděluje se celý limit každé větvi. První
snímek, přeskoky, porovnání s originálem a malé nebo nepodporované obrazy
používají přímou cestu. Nedostatečný rozpočet RAM paralelismus vypne.
Měřené zrychlení se týká průběžného toku snímků, nikoli latence přeskoku.
GPU recykluje pracovní buffery a kopíruje pouze jejich aktivní obsah.
SDK/C++ dekodér má cache vlastněnou konkrétním dekodérem s rozpočtem,
nezávislou na vlákně volajícího. Ostření a zvětšení sdílí jeden readback.
Max používá přesné lokální opravy poškozených oblastí a porovnává skutečnou
velikost s koeficientovou alternativou před přijetím predikce. Lepší úspora
není zaručena u všech klipů. AQ a ochrana textu jsou stále výchozí vypnuté.

R590.1 omezuje barevné zrnění a velké barevné bloky v Max kompresi.
Nové snímky ukládají barvy přesněji a používají prostorové čištění barev
vedené jasem a jemné čištění jasového zvonění. Nativní GPU dekodér drží
inverzi, čištění barev i převod do RGB na grafice bez dalšího čtení float
rovin procesorem. Zobrazovací filtry nemíchají sousední snímky.
Max kontroluje skutečně zobrazenou predikci proti originálu a aktuálnímu
samostatnému snímku, včetně barevného šumu a lokálních bloků. Lokální závady
opravuje přesně; nevyhovující predikci nahradí koeficientovou alternativou.
Opravuje se i rozdíl opakovaného obrazu po čerstvém přeskoku.
Čistší obraz může mít větší soubor. Velmi nízký limit může dál ztrácet
detaily a kolísat mezi snímky; Max zůstává volitelný experiment.

Video má tři volitelné metody mezisnímkové komprese:
1. Koeficienty: přesně zachová obraz samostatných snímků, menší uložení.
   Nemění původní časové kolísání kresby nezávislých snímků.
2. Oblasti: přebírá podobné bloky na stejném místě, má toleranci každého RGB
   kanálu 0–16 a limit držení podobných oblastí (výchozí 10 snímků).
3. Max komprese (experimentální): pohyb po čtvrtpixelu, lokální afinní
   predikce otočení/měřítka a kvantované barevné reziduum. Síla 0–16,
   doporučený začátek 6. Vyšší síla dovolí menší soubor a méně detailů.
   Nula je jemnější ztrátová komprese, nikoli bezztrátový režim.
   Síla není zárukou největší RGB chyby výsledného pixelu.

R560.1 přidává AVX2 pohybovou predikci na CPU a spojenou GPU cestu
predikce → přesná inverze → barvy → referenční filtr, s jediným konečným
čtením reference. Řídké reziduum se přenáší bez zbytečných nul, ale se
všemi původními hodnotami. Tím odpadá velká část přenosu koeficientů
do GPU. Zachovává pixely a data R560. Pokud GPU
nepodporuje potřebné operace, automaticky využije kanonickou CPU cestu.
Při změně rozlišení zdroje enkodér vytváří nový samostatný snímek.
Volitelná cache má přesnější paměťový limit a její alokace není podmínkou
vrácení úspěšně dekódovaného obrazu.

Předchozí R560 zrychluje barevnou rekonstrukci přes AVX2, odstraňuje nevyužité
výpočty úplných skip obrazů a jednu kopii nativního výstupního obrazu.
Opakované použití stejného keyframu má omezenou cache (32 MiB na vlákno).
Formát, zakódovaná data a pixely zachovává oproti R550. Nezvyšuje toleranci
ani nezahazuje další detaily kvůli rychlosti.

Předchozí R550 omezuje barevné zrnění a hromadění chyby reference. Slabé nejjemnější
barevné korekce potlačuje plynule, skutečné silné hrany ponechává. Běžná síla
0–6 má jemnější kvantizaci; agresivní 12–16 zůstává volitelná a může výrazně
zhoršit detaily i stabilitu v pohybu. Úspora není zárukou stejné kvality.
Volba počtu vláken 0 znamená automatický počet také v celé Max cestě.
GPU nyní provádí i čištění větších predikčních referencí. Je celočíselné
a vrací přesně stejné pixely jako CPU varianta; malý obraz využije CPU.

Výchozí Každý snímek zvlášť zůstává dostupné. Nastavení podobných oblastí
je oddělené od síly Max komprese. Při změnách obrazu se reference obnoví
nejpozději po 5 sekundách podle časových značek, případně dříve podle
zvoleného intervalu či velikosti predikce. 0 vypne jen limit počtu snímků.
Přesně statické části mají krátké přímé odkazy, bez zbytečných keyframů.
Nové řetězce mají nejvýše 16 závislostí pro omezenou cenu přeskakování.

GPU v Max režimu hledá pohyb a lokální afinní parametry; příprava rezidua
navazuje na dopředný wavelet bez mezilehlého přenosu float rovin.
Enkodér neopakuje vlastní entropický decode ani stejný třetí motion warp.
Entropie a obnova enkodérové reference zůstávají na CPU. GPU dekodér má
přesnou inverzi na GPU, případně automatický CPU fallback.
Zobrazovací doostření se aplikuje jednou až na úplně složený obraz.

Max nezaručuje stejnou kvalitu ani rychlejší encode než nezávislé snímky.
Výsledek a úspora závisejí na obsahu a nastavení. Malý rozpočet může dál
způsobovat ztrátu detailů a časové kolísání; Max je volitelný experiment.

CLI: --intra-only, --temporal-coefficients, --temporal nebo --temporal-max.
Max: --temporal-max --similar-tolerance 6 --keyframe-interval 120.
Rychlost/backend: --fast / --more-quality a --cpu / --gpu.
AQ: --enable-aq / --no-aq; ochrana textu: --protect-text 0 nebo 1.

Nová smíšená videa 136/137 a reziduum 134/16 používejte s dodaným SDK R600.2. Staré intra soubory a v1/v2
jsou podporované; C ABI zůstává 1. Jeden handle používejte z jednoho
volajícího; pro souběžné handles rozdělte celkový počet vláken.
Nastavení leží v settings.ini vedle app a při aktualizaci se zachová.
