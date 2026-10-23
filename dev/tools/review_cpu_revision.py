"""Reproducible CPU quality/speed review against preserved earlier packets.

Measures complete decode (including reconstruction), both owned output and a
reused caller buffer. Scores supplement the exported 1:1 visual comparisons.
Never overwrites reference images or baseline packets. No GPU is used.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image, ImageDraw

from review_cleanup_rates import without_cleanup


def measure(exe, packet, prefix, repeats, buffer=True):
    command = [str(exe), 'measure-decode', str(packet), str(prefix), str(repeats), '0']
    if buffer:
        command.append('buffer')
    subprocess.run(command, check=True)
    with prefix.with_suffix('.csv').open() as stream:
        values = [float(row['decode_ms']) for row in csv.DictReader(stream)]
    return dict(median_ms=float(np.median(values)), p95_ms=float(np.percentile(values, 95)),
                max_ms=max(values), repeats=len(values))


def main():
    dev = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=dev/'build-r4/fajk_cli.exe')
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--baseline-packets', type=Path, required=True)
    parser.add_argument('--medium-packets', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=60)
    parser.add_argument('--require-score-gain', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = {'22920': (1900, 60, 2220, 810), '22925': (575, 590, 935, 800),
             '23006': (1070, 700, 1800, 1350)}
    records = []
    # Do not run perceptual metrics concurrently with timing.
    for sample, crop in cases.items():
        source = dev/'samples/references'/f'{sample}-reference.png'
        for budget in [30000, 80000, 150000]:
            name = f'{sample}-{budget}'
            baseline_packet = (args.medium_packets if budget == 80000 else args.baseline_packets)/(name+'.fajk')
            packet = args.output/(name+'.fajk')
            subprocess.run([str(args.candidate), 'encode', str(source), str(packet), '--r441',
                            '--quality', '100', '--target-bytes', str(budget), '--overwrite'], check=True)
            assert packet.stat().st_size <= budget
            row = dict(sample=sample, budget=budget, bytes=packet.stat().st_size,
                       source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
            row['before_buffer'] = measure(args.baseline, baseline_packet, args.output/(name+'-before'), args.repeats)
            row['after_buffer'] = measure(args.candidate, packet, args.output/(name+'-after'), args.repeats)
            row['after_owned'] = measure(args.candidate, packet, args.output/(name+'-owned'), args.repeats)
            off = args.output/(name+'-off.fajk')
            off.write_bytes(without_cleanup(packet.read_bytes()))
            row['without_r441'] = measure(args.candidate, off, args.output/(name+'-off'), args.repeats)
            with Image.open(args.output/(name+'-after.png')) as a, Image.open(args.output/(name+'-owned.png')) as b:
                assert np.array_equal(np.array(a), np.array(b)), name+' buffer/owned mismatch'
            w, h = crop[2]-crop[0], crop[3]-crop[1]
            canvas = Image.new('RGB', (3*w, h+40), (18, 18, 18))
            draw = ImageDraw.Draw(canvas)
            files = [source, args.output/(name+'-before.png'), args.output/(name+'-after.png')]
            for column, (label, file) in enumerate(zip(['ORIGINAL', 'CPU BEFORE', 'CPU NEW'], files)):
                with Image.open(file) as image:
                    canvas.paste(image.crop(crop), (column*w, 40))
                draw.text((column*w+8, 12), label, fill='white')
            canvas.save(args.output/(name+'-comparison.png'))
            records.append(row)
            print(json.dumps(row), flush=True)
    for row in records:
        name = f"{row['sample']}-{row['budget']}"
        scores = subprocess.run([str(dev/'build-r4/fajk_metric.exe'),
            str(dev/'samples/references'/f"{row['sample']}-reference.png"),
            str(args.output/(name+'-before.png')), str(args.output/(name+'-after.png'))],
            capture_output=True, text=True, check=True)
        before, after = map(float, scores.stdout.splitlines())
        row['ssimulacra2'] = dict(before=before, after=after, gain=after-before)
    (args.output/'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    if args.require_score_gain:
        for row in records:
            gain = row['ssimulacra2']['gain']
            assert gain > .2 if row['budget'] < 150000 else gain >= -.05, row
    print('Review complete. Inspect comparison PNGs; a passing score is not visual approval.')


if __name__ == '__main__':
    main()
