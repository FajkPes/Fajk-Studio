"""Real encoded-bit maps: AQ off and additional flat savings 0/50/100.

Every panel uses one fixed source-bpp color scale. Whole-file limits include
168 container bytes. Spatial costs are transform-footprint estimates; DC and
shared headers are reported separately, never invented as per-pixel costs.
"""
import argparse, csv, json, subprocess
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from review_flat_allocation import ROIS

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exe',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--samples',nargs='+',default=['23006','22950','22920'])
    p.add_argument('--budgets',nargs='+',type=int,default=[30000,80000])
    p.add_argument('--contrast',type=int,default=100)
    p.add_argument('--vary-contrast',action='store_true')
    p.add_argument('--vary-threshold',action='store_true')
    p.add_argument('--flat-savings',type=int,default=50)
    p.add_argument('--reuse',action='store_true')
    a=p.parse_args(); a.output.mkdir(parents=True,exist_ok=True)
    samples=Path(__file__).resolve().parents[1]/'samples/references'
    records=[]
    for sample in a.samples:
        for budget in a.budgets:
            panels=[]
            choices=[(f'KONTRAST {c}% / USPORA {a.flat_savings}%',a.flat_savings,1,c) for c in (0,50,100)] if a.vary_contrast else [(label,s,on,a.contrast) for label,s,on in [('AQ OFF',0,0),('AQ ON / FLAT 0%',0,1),('AQ ON / FLAT 50%',50,1),('AQ ON / FLAT 100%',100,1)]]
            if a.vary_threshold: choices=[(f'PRAH {t}% / KONTRAST {a.contrast}%',a.flat_savings,1,a.contrast,t) for t in (0,50,100)]
            else: choices=[(*c,0) for c in choices]
            for label,strength,enabled,contrast,threshold in choices:
                prefix=a.output/(f'{sample}-{budget}-t{threshold}' if a.vary_threshold else f'{sample}-{budget}-c{contrast}' if a.vary_contrast else f'{sample}-{budget}-{strength}-{enabled}')
                if not a.reuse: subprocess.run([str(a.exe.resolve()),'inspect-allocation',str(samples/f'{sample}-reference.png'),
                    str(prefix),str(budget-168),str(contrast),'compare0',str(strength),str(enabled),str(threshold)],check=True,stdout=subprocess.DEVNULL)
                m=json.loads(Path(str(prefix)+'-allocation.json').read_text())
                rows=list(csv.DictReader(Path(str(prefix)+'-allocation.csv').open()))
                cols=(m['working_width']+m['cell']-1)//m['cell']; nrows=(m['working_height']+m['cell']-1)//m['cell']
                bits=np.array([float(r['detail_bits']) for r in rows]).reshape(nrows,cols)
                sx=m['source_width']/m['working_width']; sy=m['source_height']/m['working_height']
                widths=np.minimum(m['cell'],m['working_width']-np.arange(cols)*m['cell'])*sx
                heights=np.minimum(m['cell'],m['working_height']-np.arange(nrows)*m['cell'])*sy
                limit=4*budget*8/(m['source_width']*m['source_height'])
                t=np.clip(bits/(heights[:,None]*widths[None,:])/limit,0,1)
                color=np.empty((*t.shape,3)); low=t<.5
                color[:,:,0]=np.where(low,t*510,255-(t-.5)*50)
                color[:,:,1]=np.where(low,210-t*80,170-(t-.5)*300)
                color[:,:,2]=np.where(low,80*(1-t*2),(t-.5)*80)
                heat=Image.fromarray(np.clip(color,0,255).astype(np.uint8))
                decoded=Image.open(str(prefix)+'.png').convert('RGB')
                actual=Path(str(prefix)+'.fajk').stat().st_size; assert actual<=budget
                roi_bits={}
                for name,(x0,y0,x1,y1) in ROIS.get(sample,{}).items():
                    total=0.
                    for row in rows:
                        x=int(row['cell_x'])*m['cell']*sx; y=int(row['cell_y'])*m['cell']*sy
                        w=min(m['cell'],m['working_width']-int(row['cell_x'])*m['cell'])*sx
                        h=min(m['cell'],m['working_height']-int(row['cell_y'])*m['cell'])*sy
                        overlap=max(0,min(x+w,x1)-max(x,x0))*max(0,min(y+h,y1)-max(y,y0))
                        total+=float(row['detail_bits'])*overlap/(w*h)
                    roi_bits[name]=total
                r=dict(sample=sample,budget=budget,contrast=contrast,strength=strength,threshold=threshold,aq_enabled=bool(enabled),
                       file_bytes=actual,mapped_bits=float(bits.sum()),roi_bits=roi_bits,common_bpp_limit=limit)
                saved=Path(str(prefix)+'-savings.json')
                if saved.exists(): r['savings']=json.loads(saved.read_text())
                records.append(r)
                panel=Image.new('RGB',(640,1260),'#111111'); d=ImageDraw.Draw(panel)
                d.text((8,8),label+f' / {actual} B',fill='white')
                routing=Path(str(prefix)+'-routing.png')
                if routing.exists(): panel.paste(Image.open(routing).convert('RGB').resize((640,360),Image.Resampling.NEAREST),(0,36))
                d.text((8,402),'ROZHODNUTI AQ: CERVENA = DETAIL / ZELENA = USPORA',fill='white')
                panel.paste(heat.resize((640,360),Image.Resampling.NEAREST),(0,430))
                d.text((8,797),'SKUTECNE BITY / STEJNA BAREVNA SKALA',fill='white')
                panel.paste(decoded.resize((640,360),Image.Resampling.LANCZOS),(0,824))
                y=1192
                for name,value in roi_bits.items():
                    d.text((8,y),f'{name}: ~{value/8000:.3f} kB',fill='white'); y+=12
                    if y>1250: break
                panels.append(panel)
            combined=Image.new('RGB',(640*len(panels),1260),'#111111')
            for i,panel in enumerate(panels): combined.paste(panel,(640*i,0))
            combined.save(a.output/f'{sample}-{budget}-actual-bits.png')
            (a.output/'results.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
            print(sample,budget,'saved actual maps',flush=True)
if __name__=='__main__': main()
