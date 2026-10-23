"""Alternate two complete codec builds and require unchanged packets and RGBA.

Warmup, PNG IO and process startup are outside CLI codec timings. GPU transfer,
waits and CPU reconstruction are included. Runs are sequential with reversed
order in the second round; do not build or benchmark concurrently.
"""
import argparse
import csv
import hashlib
import json
import math
import statistics
import subprocess
from pathlib import Path
from PIL import Image, ImageChops


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ['before', 'after', 'output']:
        p.add_argument('--' + key, type=Path, required=True)
    p.add_argument('--inputs', type=Path, nargs='+', required=True)
    p.add_argument('--budgets', type=int, nargs='+', default=[30000, 80000, 150000])
    p.add_argument('--backends', nargs='+', choices=['cpu', 'gpu'], default=['cpu', 'gpu'])
    p.add_argument('--workers', type=int, default=10)
    p.add_argument('--repeats', type=int, default=8)
    p.add_argument('--rounds', type=int, default=2)
    p.add_argument('--reconstruction-ini',type=Path,help='Use saved reconstruction preferences in both builds.')
    p.add_argument('--max-pixel-delta',type=int,default=0,help='Explicit GPU-only rounding bound; packets must remain exact.')
    p.add_argument('--max-mean-delta',type=float,default=0.)
    a = p.parse_args()
    if a.rounds < 2 or a.repeats < 2 or any(b < 424 for b in a.budgets) or not 0<=a.max_pixel_delta<=1 or not 0<=a.max_mean_delta<=.00001:
        p.error('Use at least two rounds/repeats and budgets >=424.')
    a.output.mkdir(parents=True, exist_ok=True)
    sha = lambda data: hashlib.sha256(data).hexdigest()
    keys = ['encode_ms', 'decode_ms', 'encode_cpu_ms', 'decode_cpu_ms', 'encode_cycles', 'decode_cycles']
    report = dict(method=__doc__, before_sha256=sha(a.before.read_bytes()),
                  after_sha256=sha(a.after.read_bytes()), workers=a.workers,
                  repeats=a.repeats, rounds=a.rounds, cases=[], failures=[], complete=False)
    report['gpu_rounding_limits']=dict(maximum=a.max_pixel_delta,mean=a.max_mean_delta)
    if a.reconstruction_ini:
        report['reconstruction_ini']=str(a.reconstruction_ini)
        report['reconstruction_ini_sha256']=sha(a.reconstruction_ini.read_bytes())

    def save():
        (a.output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    for source in a.inputs:
        for budget in a.budgets:
            for backend in a.backends:
                tag = f'{source.stem}-{budget}-{backend}'
                case = dict(source=str(source), source_sha256=sha(source.read_bytes()),
                            budget=budget, backend=backend, runs=[])
                reference_pixels=None
                for turn in range(a.rounds):
                    for label in (['before', 'after'] if turn % 2 == 0 else ['after', 'before']):
                        prefix = a.output / f'{tag}-{turn}-{label}'
                        command = [getattr(a, label), 'measure', source, prefix, 100, a.repeats,
                                   420, a.workers, 100, 'wavelet', budget-168, 0,
                                   'r470-buffer', 50, 1, 0, 0]
                        if a.reconstruction_ini: command.append(a.reconstruction_ini)
                        command.append('--' + backend)
                        result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
                        if result.returncode:
                            raise RuntimeError(result.stdout + result.stderr)
                        rows = list(csv.DictReader(prefix.with_suffix('.csv').open()))
                        with Image.open(prefix.with_suffix('.png')) as im:
                            rgba=im.convert('RGBA');pixels = sha(rgba.tobytes())
                            if reference_pixels is None:reference_pixels=rgba.copy()
                            difference=ImageChops.difference(reference_pixels,rgba)
                            histogram=difference.histogram()
                            maximum=max(v[1] for v in difference.getextrema())
                            mean=sum((i%256)*v for i,v in enumerate(histogram))/(rgba.width*rgba.height*4)
                        case['runs'].append(dict(label=label, round=turn,
                            packet=sha(prefix.with_suffix('.fajkv').read_bytes()), pixels=pixels,
                            pixel_maximum=maximum,pixel_mean=mean,
                            **{k: statistics.median(float(r[k]) for r in rows) if k.endswith('_ms') and '_cpu_' not in k
                               else statistics.mean(float(r[k]) for r in rows) for k in keys}))
                        if prefix.with_suffix('.fajkv').stat().st_size > budget:
                            report['failures'].append(tag + ': exceeded size budget')
                        if any(int(r['gpu_fallbacks']) for r in rows):
                            report['failures'].append(tag + ': GPU fallback')
                        if backend == 'gpu' and not any(int(r['gpu_transforms']) for r in rows):
                            report['failures'].append(tag + ': no GPU transforms')
                case['packets_exact'] = len({r['packet'] for r in case['runs']}) == 1
                case['pixels_exact'] = len({r['pixels'] for r in case['runs']}) == 1
                case['pixels_within_bound']=all(r['pixel_maximum']<=(a.max_pixel_delta if backend=='gpu' else 0) and r['pixel_mean']<=(a.max_mean_delta if backend=='gpu' else 0) for r in case['runs'])
                if not case['packets_exact'] or not case['pixels_within_bound']:
                    report['failures'].append(tag + ': changed packet/pixels')
                for label in ['before', 'after']:
                    case[label] = {k: statistics.median(r[k] for r in case['runs'] if r['label'] == label) for k in keys}
                case['ratios'] = {k: case['after'][k] / case['before'][k] if case['before'][k]>0 else None for k in keys}
                report['cases'].append(case)
                save()
                print(tag, 'encode', round(case['ratios']['encode_ms'], 3),
                      'decode', round(case['ratios']['decode_ms'], 3),
                      'exact', case['packets_exact'] and case['pixels_exact'], flush=True)
    report['ratios'] = {}
    for backend in a.backends:
        selected=[c for c in report['cases'] if c['backend']==backend]
        report['ratios'][backend]={}
        for k in keys:
            if '_cpu_' in k:
                # Process-time ticks may be zero on short samples. Aggregate
                # time before dividing instead of taking log(0) or dropping it.
                denominator=sum(c['before'][k] for c in selected)
                ratio=sum(c['after'][k] for c in selected)/denominator if denominator else None
            else:
                ratio=math.exp(statistics.mean(math.log(c['ratios'][k]) for c in selected))
            report['ratios'][backend][k]=ratio
    report['cpu_time_aggregation']='ratio of summed case means; wall/cycles use geometric mean ratios'
    report['complete'] = True
    save()
    if report['failures']:
        raise SystemExit('\n'.join(report['failures']))


if __name__ == '__main__':
    main()
