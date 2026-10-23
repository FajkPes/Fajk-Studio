"""Alternate full encoders/decoders at equal container budgets, including visual review.

CLI measure times core encode/decode, excluding PNG IO and process startup.
The current single-frame container adds 168 bytes to the packet budget.
"""
import argparse
import csv
import json
import subprocess
import ctypes
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from compare_decode import assess_quality


def cpu_times():
    if sys.platform != 'win32':return None
    idle,kernel,user=ctypes.c_uint64(),ctypes.c_uint64(),ctypes.c_uint64()
    if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle),ctypes.byref(kernel),ctypes.byref(user)):return None
    return idle.value,kernel.value+user.value

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('before','after','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--samples',nargs='+',default=['22920','22925','23006'])
    p.add_argument('--references',type=Path,help='Optional source directory, otherwise samples/references.')
    p.add_argument('--budgets',type=int,nargs='+',default=[30000,80000,150000])
    p.add_argument('--flat-allocation',type=int,default=50,help='Shared flat-area savings control, 0..100.')
    p.add_argument('--repeats',type=int,default=8)
    p.add_argument('--rounds',type=int,default=2)
    p.add_argument('--max-ss2-loss',type=float,help='Fail after saving a per-frame quality regression.')
    p.add_argument('--max-edge-mae-increase',type=float,help='Allowed increase in source-edge RGB MAE (0..255 levels).')
    p.add_argument('--max-roi-mae-increase',type=float,help='Allowed increase in any named ROI RGB MAE (0..255 levels).')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    if a.rounds<2 or a.repeats<2 or any(b<=424 for b in a.budgets) or not 0<=a.flat_allocation<=100:p.error('Use at least two rounds/repeats, budgets above 424 bytes, and allocation 0..100.')
    dev=Path(__file__).resolve().parents[1];results=[]
    for sample in a.samples:
        reference_dir=a.references if a.references else dev/'samples/references'
        reference=reference_dir/(sample+'-reference.png')
        if not reference.exists():reference=reference_dir/(sample+'.png')
        for budget in a.budgets:
            times={k:[] for k in ('before','after')}
            rounds={k:[] for k in ('before','after')}
            for turn in range(a.rounds):
                for label in (('before','after') if turn%2==0 else ('after','before')):
                    prefix=a.output/f'{sample}-{budget}-{label}-{turn}'
                    cpu_start=cpu_times()
                    subprocess.run([str(getattr(a,label).resolve()),'measure',str(reference),str(prefix),'100',str(a.repeats),'420','0','100','wavelet',str(budget-168),'0','r462-buffer',str(a.flat_allocation)],check=True)
                    cpu_end=cpu_times()
                    with prefix.with_suffix('.csv').open() as f:entries=list(csv.DictReader(f))
                    times[label].extend(entries)
                    run={'round':turn,'encode_median_ms':float(np.median([float(v['encode_ms']) for v in entries])),
                         'decode_median_ms':float(np.median([float(v['decode_ms']) for v in entries]))}
                    if cpu_start and cpu_end and cpu_end[1]>cpu_start[1]:
                        run['system_cpu_busy_percent_including_export']=100*(1-(cpu_end[0]-cpu_start[0])/(cpu_end[1]-cpu_start[1]))
                    rounds[label].append(run)
                    assert prefix.with_suffix('.fajkv').stat().st_size<=budget
            row={'sample':sample,'budget':budget,'flat_allocation':a.flat_allocation}
            for label,entries in times.items():
                row[label]={k:float(np.median([float(v[k]) for v in entries])) for k in ('encode_ms','decode_ms')}
                row[label]['decode_p95_ms']=float(np.percentile([float(v['decode_ms']) for v in entries],95))
                row[label]['encode_p95_ms']=float(np.percentile([float(v['encode_ms']) for v in entries],95))
                row[label]['rounds']=rounds[label]
                row[label]['file_bytes']=(a.output/f'{sample}-{budget}-{label}-0.fajkv').stat().st_size
            images=[reference]+[a.output/f'{sample}-{budget}-{label}-0.png' for label in ('before','after')]
            row.update(assess_quality(images[0],images[1],images[2],dev/'build-r4/fajk_metric.exe'))
            with Image.open(images[1]) as im:before=np.asarray(im).copy()
            with Image.open(images[2]) as im:row['identical_pixels']=bool(np.array_equal(before,np.asarray(im)))
            # Original source coordinates; independent checks prevent a global
            # metric gain from concealing damaged glyphs, faces or dark hair.
            roi_boxes={
                '22920':{'face':(1980,130,2070,245),'dark_hair':(1935,150,1978,265),'cap_edge':(1925,65,2080,145),'ui_text':(2090,155,2400,275)},
                '22950':{'car':(650,730,1660,1300),'wall':(1660,180,2250,800),'ui_text':(30,1080,380,1140)},
                '22925':{'detail':(575,590,1095,970)},
                '23006':{'character':(900,620,1580,1380),'tutorial_text':(20,55,520,200),'window_edges':(650,30,1250,550)}
            }.get(sample,{})
            ref=np.asarray(Image.open(images[0]).convert('RGB')).astype(np.int16)
            candidate=np.asarray(Image.open(images[2]).convert('RGB')).astype(np.int16)
            previous=before[:,:,:3].astype(np.int16)
            row['rois']={}
            for label,(x0,y0,x1,y1) in roi_boxes.items():
                region=ref[y0:y1,x0:x1]
                if not region.size:raise ValueError(f'Empty ROI: {sample}/{label}')
                row['rois'][label]={kind:float(np.abs(array[y0:y1,x0:x1]-region).mean()) for kind,array in (('before',previous),('after',candidate))}
            crop={'22920':(1900,65,2340,520),'22950':(650,730,1660,1300),'22925':(575,590,1095,970),'23006':(1070,700,1800,1350)}.get(sample)
            if crop:
                w,h=crop[2]-crop[0],crop[3]-crop[1];canvas=Image.new('RGB',(3*w,h+40),(18,18,18));draw=ImageDraw.Draw(canvas)
                for i,(label,path) in enumerate(zip(['ORIGINAL','BEFORE','AFTER'],images)):
                    with Image.open(path) as im:canvas.paste(im.crop(crop),(i*w,40))
                    draw.text((i*w+8,12),label,fill='white')
                canvas.save(a.output/f'{sample}-{budget}-comparison.png')
            results.append(row);(a.output/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8');print(json.dumps(row),flush=True)
            if a.max_ss2_loss is not None and row['ss2_after']<row['ss2_before']-a.max_ss2_loss:raise RuntimeError(f'Visual metric regression: {sample}/{budget}')
            if a.max_edge_mae_increase is not None and row['edge_band_mae_after']>row['edge_band_mae_before']+a.max_edge_mae_increase:raise RuntimeError(f'Edge regression: {sample}/{budget}')
            if a.max_roi_mae_increase is not None:
                for label,values in row['rois'].items():
                    if values['after']>values['before']+a.max_roi_mae_increase:raise RuntimeError(f'ROI regression: {sample}/{budget}/{label}')

if __name__=='__main__':main()
