"""Compare real AQ off / balanced / maximum at identical whole-file limits.

The saved PNGs use identical reconstruction settings. Timings include the core
codec, not PNG IO or startup; this ordered visual run is not an isolated speed benchmark.
"""
import argparse,csv,json,statistics,subprocess
from pathlib import Path
from PIL import Image,ImageDraw
from compare_decode import assess_quality
from review_flat_allocation import ROIS
import numpy as np

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--exe',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--flat-savings',type=int,default=50)
 p.add_argument('--samples',nargs='+',default=['22950','22920','23006'])
 p.add_argument('--budgets',nargs='+',type=int,default=[30000,80000]);p.add_argument('--repeats',type=int,default=8)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 dev=Path(__file__).resolve().parents[1];records=[]
 for sample in a.samples:
  reference=dev/'samples/references'/(sample+'-reference.png')
  ref=np.asarray(Image.open(reference).convert('RGB')).astype(np.int16)
  for budget in a.budgets:
   paths=[reference];labels=['ORIGINAL']
   for label,control,enabled in [('AQ OFF',50,0),('AQ 50%',50,1),('AQ 100%',100,1)]:
    prefix=a.output/f'{sample}-{budget}-{control}-{enabled}'
    subprocess.run([str(a.exe.resolve()),'measure',str(reference),str(prefix),'100',str(a.repeats),'420','0','100','wavelet',str(budget-168),'0','r465-buffer',str(control),str(enabled),str(a.flat_savings)],check=True,stdout=subprocess.DEVNULL)
    frame=prefix.with_suffix('.png');container=prefix.with_suffix('.fajkv')
    assert container.stat().st_size<=budget
    timings=list(csv.DictReader(prefix.with_suffix('.csv').open()))
    decoded=np.asarray(Image.open(frame).convert('RGB')).astype(np.int16)
    row=dict(sample=sample,budget=budget,aq_enabled=bool(enabled),control=control,flat_savings=a.flat_savings,file_bytes=container.stat().st_size,
     encode_ms=statistics.median(float(t['encode_ms']) for t in timings),decode_ms=statistics.median(float(t['decode_ms']) for t in timings))
    row.update(assess_quality(reference,frame,frame,dev/'build-r4/fajk_metric.exe'))
    row['roi_mae']={name:float(np.abs(ref[y0:y1,x0:x1]-decoded[y0:y1,x0:x1]).mean()) for name,(x0,y0,x1,y1) in ROIS.get(sample,{}).items()}
    records.append(row);print(json.dumps(row),flush=True)
    (a.output/'results.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    paths.append(frame);labels.append(label+f' / {container.stat().st_size} B')
   crop={'22950':(650,730,1660,1300),'22920':(1900,65,2340,520),'23006':(1070,700,1800,1350)}[sample]
   w,h=crop[2]-crop[0],crop[3]-crop[1]
   canvas=Image.new('RGB',(w*4,h+40),'#111111');draw=ImageDraw.Draw(canvas)
   for i,(label,path) in enumerate(zip(labels,paths)):
    canvas.paste(Image.open(path).convert('RGB').crop(crop),(i*w,40));draw.text((i*w+8,10),label,fill='white')
   canvas.save(a.output/f'{sample}-{budget}-comparison.png')
if __name__=='__main__': main()
