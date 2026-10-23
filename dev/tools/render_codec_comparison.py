"""Render recorded codec comparison as a local, synchronized zoom/pan viewer."""
import argparse,json,html,csv
from pathlib import Path
from PIL import Image,ImageDraw

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);a=p.parse_args()
    report=json.loads((a.directory/'results.json').read_text());rows=report['results']
    ref=Image.open(a.directory/'reference.png').convert('RGB')
    for budget in sorted({r['budget'] for r in rows}):
        entries={r['codec']:r for r in rows if r['budget']==budget}
        images={'Original':ref,**{name:Image.open(a.directory/r['png']).convert('RGB') for name,r in entries.items()}}
        # Source coordinates, identical native-scale crops in every column.
        for label,box in [('dog',(1620,420,2460,1800)),('fur',(1760,470,2272,982)),
                          ('text',(150,980,1320,1260)),('eyes',(1770,620,2260,960))]:
            w,h=box[2]-box[0],box[3]-box[1];canvas=Image.new('RGB',(4*w,h+64),'#111111');d=ImageDraw.Draw(canvas)
            for i,name in enumerate(('Original','FAJK','AVIF','JPEG XL')):
                r=entries.get(name);d.text((i*w+8,8),name+(f" / {r['bytes']} B" if r else ''),fill='white')
                if r:d.text((i*w+8,28),f"ENC {r['encode']['median_ms']:.2f} ms / DEC {r['decode']['median_ms']:.2f} ms",fill='white')
                canvas.paste(images[name].crop(box),(i*w,64))
            canvas.save(a.directory/f'{budget}-{label}-comparison.png')
    export=[]
    for r in rows:
        export.append(dict(codec=r['codec'],budget=r['budget'],bytes=r['bytes'],encode_median_ms=r['encode']['median_ms'],
            encode_p95_ms=r['encode']['p95_ms'],decode_median_ms=r['decode']['median_ms'],decode_p95_ms=r['decode']['p95_ms'],
            ssimulacra2=r['ssimulacra2'],parameter=r.get('parameter'),external_search_ms=r.get('search_ms'),
            decode_native_median_ms=r.get('decode_native',{}).get('median_ms')))
    with (a.directory/'timings.csv').open('w',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,fieldnames=list(export[0]));writer.writeheader();writer.writerows(export)
    data=json.dumps(report,ensure_ascii=False).replace('</',r'<\/')
    template=r'''<!doctype html><html lang="cs"><meta charset="utf-8"><title>FAJK / AVIF / JPEG XL</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#111317;color:#eceef2;font:14px system-ui}
header{padding:18px 24px;border-bottom:1px solid #343943}h1{font-size:22px;margin:0 0 8px}
button,select{padding:8px 12px;background:#252a33;color:inherit;border:1px solid #515967;border-radius:5px;cursor:pointer}
button.active{background:#2a5468}nav{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin:12px 0}
.note{color:#b7c0cc;max-width:1350px;line-height:1.5}.panes{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:2px}
.label{min-height:86px;padding:12px;background:#20242b;line-height:1.5}.label strong{font-size:17px}
.viewport{height:65vh;overflow:hidden;position:relative;background:#090a0d;cursor:grab;touch-action:none}
.viewport img{position:absolute;max-width:none;transform-origin:0 0;image-rendering:auto;user-select:none;pointer-events:none}
details{margin:18px 24px}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}th,td{padding:10px;text-align:right;border-bottom:1px solid #343943}th:first-child,td:first-child{text-align:left}a{color:#84c6e7}
</style><header><h1>FAJK R470 / AVIF / JPEG XL — stejné limity souboru</h1>
<div id="source" class="note"></div><nav id="budgets"></nav><nav>
<button onclick="view('fit')">Celý obraz</button><button onclick="view('dog')">Pes</button><button onclick="view('eyes')">Obličej / srst 1:1</button><button onclick="view('text')">Text 1:1</button>
<button onclick="zoom(1.25)">+</button><button onclick="zoom(.8)">−</button><span id="scale"></span></nav>
<div class="note">Kolečko zvětšuje kolem kurzoru, tažení posouvá všechny čtyři obrazy společně. Měří se celý soubor včetně obalu. AVIF: speed 6, AOM, 4:2:0; JXL: effort 7, XYB, automatické převzorkování. Při 30 kB AVIF používá výslovně označené menší pracovní rozlišení. Náhledy jsou skutečné dekódované PNG.</div></header>
<main class="panes" id="panes"></main><details open><summary>Naměřené časy a podmínky</summary><p class="note">Medián a p95 z 8 opakování, 10 nastavených vláken. Bez PNG I/O a startu procesu. FAJK encode zahrnuje vlastní hledání velikosti; externí hledání AVIF/JXL je uvedeno zvlášť. FAJK decode používá připravený RGBA buffer, AVIF/JXL zahrnují alokaci a API kopie. AVIF při 30 kB: hlavní decode zahrnuje i zvětšení na 4K. Samostatné pozdější měření nativního decode bez zvětšení je ve vlastním sloupci. Presety nejsou výkonově vyrovnané. Výsledky jednoho 4K obrázku nejsou obecný žebříček kodeků.</p><table><thead><tr><th>Kodek / limit</th><th>Soubor B</th><th>Encode ms / p95</th><th>Decode ms / p95</th><th>Nativní AVIF decode ms</th><th>SSIMULACRA2</th><th>Externí hledání s</th></tr></thead><tbody id="times"></tbody></table></details>
<script>const R=__DATA__;let budget=30000,s=.25,x=0,y=0,drag=null;
const names=['Original','FAJK','AVIF','JPEG XL'];
const panes=document.getElementById('panes');
names.forEach((n,i)=>{const pane=document.createElement('section');pane.innerHTML='<div class="label"></div><div class="viewport"><img draggable="false"></div>';panes.append(pane);const v=pane.lastChild;
 v.addEventListener('wheel',e=>{e.preventDefault();const b=v.getBoundingClientRect();zoom(Math.exp(-e.deltaY*.0015),e.clientX-b.left,e.clientY-b.top)},{passive:false});
 v.addEventListener('pointerdown',e=>{drag={px:e.clientX,py:e.clientY,x,y};v.setPointerCapture(e.pointerId)});
 v.addEventListener('pointermove',e=>{if(drag){x=drag.x+e.clientX-drag.px;y=drag.y+e.clientY-drag.py;paint()}});v.addEventListener('pointerup',()=>drag=null);v.addEventListener('pointercancel',()=>drag=null);
});
function paint(){document.querySelectorAll('.viewport img').forEach(i=>i.style.transform=`translate(${x}px,${y}px) scale(${s})`);document.getElementById('scale').textContent=Math.round(s*100)+' %'}
function zoom(f,cx,cy){const v=document.querySelector('.viewport');cx=cx??v.clientWidth/2;cy=cy??v.clientHeight/2;const next=Math.max(.04,Math.min(5,s*f));x=cx-(cx-x)*next/s;y=cy-(cy-y)*next/s;s=next;paint()}
function view(which){const v=document.querySelector('.viewport');if(which==='fit'){s=Math.min(v.clientWidth/R.size[0],v.clientHeight/R.size[1]);x=(v.clientWidth-R.size[0]*s)/2;y=(v.clientHeight-R.size[1]*s)/2}else{const center={dog:[2040,1050,.38],eyes:[2015,770,1],text:[730,1110,1]}[which];s=center[2];x=v.clientWidth/2-center[0]*s;y=v.clientHeight/2-center[1]*s}paint()}
function load(b){budget=b;[...panes.children].forEach((p,i)=>{const n=names[i],r=R.results.find(r=>r.budget===b&&r.codec===n);p.querySelector('img').src=r?r.png:'reference.png';let extra='';if(r?.codec==='AVIF')extra=' / pracovní '+r.settings.encoded_size.join('×');if(r?.codec==='JPEG XL')extra=' / resampling '+r.settings.resampling;p.querySelector('.label').innerHTML=`<strong>${n}</strong>${r?' / '+r.bytes.toLocaleString('cs-CZ')+' B':''}<br>${r?'ENC '+r.encode.median_ms.toFixed(2)+' ms · DEC '+r.decode.median_ms.toFixed(2)+' ms'+extra:'Původní RGB / stejný zdroj'}`});document.querySelectorAll('#budgets button').forEach(b=>b.classList.toggle('active',Number(b.dataset.b)===budget));view('dog')}
[...new Set(R.results.map(r=>r.budget))].forEach(b=>{const q=document.createElement('button');q.textContent=(b/1000)+' kB';q.dataset.b=b;q.onclick=()=>load(b);document.getElementById('budgets').append(q)});
document.getElementById('source').textContent=`Zdroj 22885, ${R.size.join(' × ')} px. libavif ${R.libavif}; ${R.avif_codecs}; libjxl ${Math.floor(R.libjxl_version/1000000)}.${Math.floor(R.libjxl_version/1000)%1000}.${R.libjxl_version%1000}.`;
R.results.forEach(r=>{const tr=document.createElement('tr');tr.innerHTML=`<td>${r.codec} / ${r.budget/1000} kB</td><td>${r.bytes}</td><td>${r.encode.median_ms.toFixed(2)} / ${r.encode.p95_ms.toFixed(2)}</td><td>${r.decode.median_ms.toFixed(2)} / ${r.decode.p95_ms.toFixed(2)}</td><td>${r.decode_native?r.decode_native.median_ms.toFixed(2):'—'}</td><td>${r.ssimulacra2.toFixed(2)}</td><td>${r.search_ms?(r.search_ms/1000).toFixed(2):'zahrnuto'}</td>`;document.getElementById('times').append(tr)});load(30000);
</script></html>'''
    (a.directory/'comparison.html').write_text(template.replace('__DATA__',data),encoding='utf-8')
    print('Rendered side-by-side viewer, correct native crops and timings.csv')
if __name__=='__main__':main()
