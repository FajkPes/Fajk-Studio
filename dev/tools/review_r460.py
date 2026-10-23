"""R460 review: identical approved controls, byte limits and real decoded PNGs.

Run baseline and candidate sequentially, never concurrently with benchmarking.
Encode wall time includes CLI startup and file IO; decode timings do not.
"""
import argparse
import csv
import json
import subprocess
import time
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

dev = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--baseline', type=Path)
p.add_argument('--baseline-label', default='R450 / approved controls')
p.add_argument('--candidate-label', default='R460')
p.add_argument('--max-ss2-loss', type=float, help='Fail on a larger per-frame loss after saving the review.')
p.add_argument('--repeats', type=int, default=30)
p.add_argument('--samples', nargs='+', default=['22920', '22925', '23006'])
p.add_argument('--budgets', nargs='+', type=int, default=[30000, 80000, 150000])
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
records = []
for sample in a.samples:
    source = dev/'samples/references'/f'{sample}-reference.png'
    if not source.exists(): source = dev/'samples/references'/f'{sample}.png'
    if not source.exists(): source = dev/'samples'/f'Snímek obrazovky ({sample}).png'
    for budget in a.budgets:
        name = f'{sample}-{budget}'
        packet = a.output/(name+'.fajk')
        start = time.perf_counter()
        subprocess.run([str(a.exe), 'encode', str(source), str(packet), '--quality', '100',
                        '--target-bytes', str(budget), '--r441-intensity', '50',
                        '--shape-intensity', '200', '--contour-intensity', '0',
                        '--stronger-tangents', '0', '--antialias', '50', '--contour-contrast', '0',
                        '--protect-text', '1', '--flat-savings', '0', '--overwrite'], check=True)
        encode_wall = (time.perf_counter()-start)*1000
        assert packet.stat().st_size <= budget
        prefix = a.output/name
        subprocess.run([str(a.exe), 'measure-decode', str(packet), str(prefix), str(a.repeats), '0', 'buffer'], check=True)
        with prefix.with_suffix('.csv').open() as stream:
            times = [float(r['decode_ms']) for r in csv.DictReader(stream)]
        row = dict(sample=sample,budget=budget,bytes=packet.stat().st_size,
                   encode_process_ms=encode_wall,median_ms=float(np.median(times)),p95_ms=float(np.percentile(times,95)))
        if a.baseline:
            files = [source, a.baseline/(name+'.png'), prefix.with_suffix('.png')]
            result = subprocess.run([str(dev/'build-r4/fajk_metric.exe'), *map(str, files)], capture_output=True,text=True,check=True)
            row['ss2_before'], row['ss2_after'] = map(float,result.stdout.splitlines())
            with Image.open(source) as original:
                reference=np.asarray(original.convert('RGB')).astype(np.int16)
            before=np.asarray(Image.open(files[1]).convert('RGB')).astype(np.int16)
            after=np.asarray(Image.open(files[2]).convert('RGB')).astype(np.int16)
            edge=np.zeros(reference.shape[:2],bool)
            edge[:,1:] |= np.max(np.abs(reference[:,1:]-reference[:,:-1]),axis=2)>24
            edge[1:,:] |= np.max(np.abs(reference[1:]-reference[:-1]),axis=2)>24
            padded=np.pad(edge,3)
            band=np.zeros_like(edge)
            for dy in range(7):
                for dx in range(7):
                    band |= padded[dy:dy+edge.shape[0],dx:dx+edge.shape[1]]
            for label,decoded in [('before',before),('after',after)]:
                difference=np.abs(decoded-reference)
                row[f'rgb_mae_{label}']=float(difference.mean())
                row[f'edge_band_mae_{label}']=float(difference[band].mean()) if band.any() else 0.
            if sample=='22920':
                glyph=(reference.min(axis=2)>150)&(np.ptp(reference,axis=2)<32)&band
                glyph[:,:min(2160,reference.shape[1])]=False
                glyph[min(800,reference.shape[0]):,:]=False
                for label,decoded in [('before',before),('after',after)]:
                    row[f'ui_glyph_mae_{label}']=float(np.abs(decoded-reference)[glyph].mean()) if glyph.any() else 0.
            with Image.open(source) as full:
                width,height=full.size
            crop = {'22920': (1900,60,2220,810), '22925': (575,590,935,800), '23006': (1070,700,1800,1350),
                    '22950': (700,600,1450,1050)}.get(sample, (width//4,height//4,width//4+min(700,width//2),height//4+min(600,height//2)))
            w,h = crop[2]-crop[0],crop[3]-crop[1]
            canvas=Image.new('RGB',(w*3,h+40),(18,18,18));draw=ImageDraw.Draw(canvas)
            for i,(file,label) in enumerate(zip(files,['ORIGINAL',a.baseline_label,a.candidate_label])):
                with Image.open(file) as image: canvas.paste(image.crop(crop),(i*w,40))
                draw.text((i*w+8,12),label,fill='white')
            canvas.save(a.output/(name+'-comparison.png'))
        records.append(row)
        (a.output/'results.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
        print(json.dumps(row),flush=True)
        if a.baseline and a.max_ss2_loss is not None:
            assert row['ss2_after']>=row['ss2_before']-a.max_ss2_loss, f'Visual metric regression: {name}'
