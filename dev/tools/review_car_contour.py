"""Paired codec comparison on a documented approximate lower-car contour ROI."""
import argparse,csv,json,statistics,subprocess
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for k in ('before','after','output'): p.add_argument('--'+k,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    source=Path(__file__).resolve().parents[1]/'samples/references/22950-reference.png'
    original=Image.open(source).convert('RGB'); ref=np.asarray(original).astype(np.int16)
    mask=Image.new('L',original.size); draw=ImageDraw.Draw(mask)
    points=[(650,1210),(680,1260),(800,1290),(925,1296),(1080,1285),(1210,1270),
            (1300,1230),(1385,1195),(1450,1150),(1550,1075),(1625,1040)]
    draw.line(points,fill=255,width=16); selected=np.asarray(mask)>0
    mask.save(a.output/'lower-contour-roi.png'); records=[]
    for budget in (30000,80000):
        panels=[('ORIGINAL',original)]
        for contrast in (50,100):
            for name,exe in [('BEFORE',a.before),('AFTER',a.after)]:
                prefix=a.output/f'{budget}-c{contrast}-{name}'
                subprocess.run([str(exe.resolve()),'measure',str(source),str(prefix),'100','8','420','0','100','wavelet',
                                str(budget-168),'0','r465-buffer',str(contrast),'1','50'],check=True,stdout=subprocess.DEVNULL)
                image=Image.open(prefix.with_suffix('.png')).convert('RGB'); frame=np.asarray(image).astype(np.int16)
                times=list(csv.DictReader(prefix.with_suffix('.csv').open()))
                r=dict(budget=budget,contrast=contrast,version=name,bytes=prefix.with_suffix('.fajkv').stat().st_size,
                       lower_contour_rgb_mae=float(np.abs(frame-ref)[selected].mean()),
                       encode_ms=statistics.median(float(t['encode_ms']) for t in times),
                       decode_ms=statistics.median(float(t['decode_ms']) for t in times))
                records.append(r); print(json.dumps(r),flush=True)
                panels.append((f'{name} / KONTRAST {contrast}%',image))
        crop=(650,1080,1500,1310);w,h=crop[2]-crop[0],crop[3]-crop[1]
        canvas=Image.new('RGB',(w,len(panels)*(h+32)),'#111111');draw=ImageDraw.Draw(canvas)
        for i,(label,image) in enumerate(panels):
            draw.text((8,i*(h+32)+8),label+' / ALL PANELS DISPLAYED 3x BRIGHTER',fill='white')
            bright=Image.fromarray(np.clip(np.asarray(image.crop(crop)).astype(np.uint16)*3,0,255).astype(np.uint8))
            canvas.paste(bright,(0,i*(h+32)+32))
        canvas.save(a.output/f'{budget}-lower-contour.png')
        (a.output/'results.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
if __name__=='__main__': main()
