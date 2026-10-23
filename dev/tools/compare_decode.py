"""Alternate two CPU/GPU decoder builds on identical packets; exclude PNG export and IO.

Use compatible packets both binaries can read. This measures decoder changes,
not the combined effect of a new encoder, and does not require equal pixels.
"""
import argparse
import csv
import json
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image


def assess_quality(reference_path, before_path, after_path, metric):
    same = Path(before_path).resolve() == Path(after_path).resolve()
    candidates = [str(before_path)] if same else [str(before_path), str(after_path)]
    scores = subprocess.check_output([str(metric), str(reference_path), *candidates], text=True).splitlines()
    reference = np.asarray(Image.open(reference_path).convert('RGB')).astype(np.int16)
    result = {'ss2_before': float(scores[0]), 'ss2_after': float(scores[0 if same else 1])}
    edge = np.zeros(reference.shape[:2], bool)
    edge[:, 1:] |= np.max(np.abs(reference[:, 1:] - reference[:, :-1]), axis=2) > 24
    edge[1:] |= np.max(np.abs(reference[1:] - reference[:-1]), axis=2) > 24
    padded = np.pad(edge, 3)
    band = np.zeros_like(edge)
    for y in range(7):
        for x in range(7):
            band |= padded[y:y + edge.shape[0], x:x + edge.shape[1]]
    for label, path in [('before', before_path), ('after', after_path)]:
        if same and label == 'after':
            result['rgb_mae_after'] = result['rgb_mae_before']
            result['edge_band_mae_after'] = result['edge_band_mae_before']
            continue
        image = np.asarray(Image.open(path).convert('RGB')).astype(np.int16)
        if image.shape != reference.shape:
            raise ValueError('Reference and decoded dimensions differ.')
        error = np.abs(image - reference)
        result['rgb_mae_' + label] = float(error.mean())
        result['edge_band_mae_' + label] = float(error[band].mean()) if band.any() else 0.
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('before', 'after', 'packets', 'output'):
        parser.add_argument('--' + flag, type=Path, required=True)
    parser.add_argument('--samples', nargs='+', default=['22920', '22925', '23006'])
    parser.add_argument('--budgets', nargs='+', type=int, default=[30000, 80000, 150000])
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--repeats', type=int, default=24)
    parser.add_argument('--workers', type=int, default=0)
    parser.add_argument('--backend', choices=['cpu', 'gpu'], default='cpu')
    parser.add_argument('--require-exact-pixels', action='store_true')
    parser.add_argument('--references', type=Path, help='Optional original PNG directory for quality assessment.')
    parser.add_argument('--metric', type=Path, default=Path(__file__).resolve().parents[1] / 'build-r4/fajk_metric.exe')
    args = parser.parse_args()
    if args.rounds < 2 or args.repeats < 2:
        parser.error('Use at least two alternating rounds and two timed decodes.')
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    for sample in args.samples:
        for budget in args.budgets:
            name = f'{sample}-{budget}'
            packet = args.packets / (name + '.fajk')
            times = {'before': [], 'after': []}
            medians = {'before': [], 'after': []}
            for repeat in range(args.rounds):
                order = ['before', 'after'] if repeat % 2 == 0 else ['after', 'before']
                for label in order:
                    prefix = args.output / f'{name}-{label}-{repeat}'
                    subprocess.run([str(getattr(args, label).resolve()), 'measure-decode', str(packet),
                                    str(prefix), str(args.repeats), str(args.workers), 'buffer', '--'+args.backend], check=True, capture_output=True)
                    with prefix.with_suffix('.csv').open() as stream:
                        measured = [float(row['decode_ms']) for row in csv.DictReader(stream)]
                    times[label].extend(measured)
                    medians[label].append(float(np.median(measured)))
            row = dict(sample=sample, budget=budget, packet=str(packet), workers=args.workers, backend=args.backend)
            reference_pixels = None
            row['identical_pixels'] = True
            for repeat in range(args.rounds):
                for label in ['before', 'after']:
                    with Image.open(args.output / f'{name}-{label}-{repeat}.png') as image:
                        pixels = np.asarray(image.convert('RGBA'))
                    if reference_pixels is None:
                        reference_pixels = pixels
                    else:
                        row['identical_pixels'] &= bool(np.array_equal(reference_pixels, pixels))
            for label, measured in times.items():
                row[label] = dict(median_ms=float(np.median(measured)),
                                  p95_ms=float(np.percentile(measured, 95)),
                                  round_medians_ms=medians[label], repeats=len(measured))
            row['median_change_percent'] = 100 * (row['after']['median_ms'] / row['before']['median_ms'] - 1)
            if args.references:
                reference = args.references / (sample + '-reference.png')
                if not reference.exists():
                    reference = args.references / (sample + '.png')
                if not reference.exists():
                    reference = args.references.parent / f'Snímek obrazovky ({sample}).png'
                row['quality'] = assess_quality(reference, args.output / (name + '-before-0.png'), args.output / (name + '-after-0.png'), args.metric)
            records.append(row)
            (args.output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
            print(json.dumps(row), flush=True)
            if args.require_exact_pixels and not row['identical_pixels']:
                raise RuntimeError(f'Decode pixels changed: {name}/{args.backend}')


if __name__ == '__main__':
    main()
