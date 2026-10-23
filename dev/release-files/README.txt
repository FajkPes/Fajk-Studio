Fajk Studio R500 — 0.11.500.1, Windows x64

Oprava .1: rychlejší vektorové porovnávání podobných oblastí se stejnými
pakety; SDK recykluje pomocný obraz pro výstup s výplní řádků (do 64 MiB
na handle). Souvislý výstup zůstává přímo v bufferu volajícího. Přibyl
nativní test skutečné DLL, který běží i pod AddressSanitizerem.

Spusťte Spustit Fajk Studio.cmd.

Enkodér a dekodér: společná volba R500 CPU / R500 GPU platí pro kompresi,
náhled i přehrávání. GPU používá současné algoritmy R470; potřebné shadery
jsou uvnitř programu. Při nedostupnosti GPU se použije CPU. Entropie a některé
filtry zůstávají na CPU; cesty se vybírají podle velikosti a rozpočtu vláken.
Počet vláken neomezujte jen pro aktivaci dalšího GPU filtru.

Fast / More quality jsou samostatné režimy pro fotografie i video.
Fast vynechává drahé hledání a může ztratit jemné detaily při stejné velikosti.
More quality věnuje více času hledání parametrů. Limit bajtů zůstává zachovaný.
CLI: --fast / --more-quality, --cpu / --gpu.

Adaptivní kvantizace i ochrana textu jsou výchozí vypnuté, zůstávají nastavitelné.
Při prvním převzetí starého INI do R500 se tyto dvě volby vypnou. Následné
změny se ukládají do settings.ini mimo složku app a další build je zachová.
CLI zapnutí AQ: --enable-aq; vypnutí: --no-aq. --aq N mění jen sílu.
Text: --protect-text 0 nebo 1.

Video: výchozí Každý snímek zvlášť (původní) zachovává nezávislou kompresi.
Volitelná mezisnímková komprese odkazuje přímo na keyframe, bez řetězení delt.
Adaptivní interval keyframu: výchozí 120 uložených snímků, 0 vypne věkový limit.
Při toleranci 0 se ukládají přesné rozdíly zdroje vůči zdrojovému keyframu.
Shodné snímky a změny nejvýše 1 % pixelů mohou interval prodlužovat.

Tolerance podobnosti 1–16 dovolí převzít blok 16×16, pouze když rozdíl každého
RGB kanálu každého pixelu nepřesáhne práh. Alfa se musí shodovat. Jde o úrovně
0–255, nikoli procenta. Limit držení je 1–120 uložených snímků, výchozí 10.
Změněné bloky mají přesné číselné korekce vůči dekódovanému keyframu.
Velká změna nebo drahé korekce vytvoří celý nezávislý snímek. Fast omezuje
rozsah pokusu podle rozpočtu bajtů a ukončuje zjevně drahý pokus dříve podle
osmi skutečně zakódovaných bloků. More quality může zkoušet déle.
Při rychlém pohybu kamery nemusí temporal režim šetřit data ani čas;
pohybová kompenzace není implementovaná. Režim je volitelný.
CLI: --temporal --keyframe-interval 120 --similar-tolerance 3 --similar-hold 10.

Prototypy se ztrátovou kompresí oddělených oblastí, které vytvářely mřížku
a barevné duchy, nejsou součástí R500. Aktualizované bloky se nekomprimují
druhým ztrátovým obrazovým enkodérem. Převzaté bloky stále nesou původní
chybu keyframu a případnou povolenou toleranci; celý soubor není bezeztrátový.

Finální ostření 0–100 % najdete pod obrazem. Aplikuje se až na hotový,
vyčištěný a zkonturovaný snímek, včetně složení temporal obrazu. Síla neslábne
s velikostí souboru. Výchozí 0 % vynechá průchod navíc. CPU AVX2 a GPU ostření
jsou celočíselně shodné; s více CPU vlákny bývá rychlejší AVX2. Maximum nemusí
zlepšit percepční kvalitu. Kontury zůstávají účinné nad 80/150 kB; intenzita 0
je vypne. Ovladače finálního ostření mění zobrazení bez opětovné komprese.

Použijte dodanou SDK R500; C ABI zůstává 1. Korekce 133 vyžadují R500 dekodér,
přesné rozdíly 132 alespoň 0.11.490.2. Staré intra soubory jsou podporované.
CPU/GPU mohou mít drobné rozdíly zaokrouhlení. Jeden SDK handle používejte
z jednoho volajícího; při souběhu rozdělte rozpočet vláken mezi handles.
Rychlost závisí na obsahu, velikosti, nastavení a ostatní zátěži počítače.
