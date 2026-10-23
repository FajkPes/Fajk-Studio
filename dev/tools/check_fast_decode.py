"""Measure CPU decode and check owned/buffer/worker equality on existing packets.

Keeps the old reference manifest intact. --baseline compares a previous fast
decoder, not the much slower historical R441 reference reconstruction.
"""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import numpy as np
from PIL import Image


def run(exe, packet, prefix, repeats, workers=0, direct=False):
    args = [str(exe.resolve()), 'measure-decode', str(packet), str(prefix), str(repeats), str(workers)]
    if direct:
        args.append('buffer')
    subprocess.run(args, check=True)
    with prefix.with_suffix('.csv').open() as f:
        samples = np.array([float(row['decode_ms']) for row in csv.DictReader(f)])
    return dict(median_ms=float(np.median(samples)), p95_ms=float(np.percentile(samples, 95)),
                max_ms=float(samples.max()), repeats=len(samples))


def main():
    dev = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=dev/'build-r4/fajk_cli.exe')
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--packets', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=40)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = []
    for sample in ['22920', '22925', '23006']:
        for budget in [30000, 150000]:
            name = f'{sample}-{budget}'
            packet = args.packets/(name+'.fajk')
            entry = dict(name=name, bytes=packet.stat().st_size)
            assert entry['bytes'] <= budget
            for mode, direct, workers, repeats in [('owned', False, 0, args.repeats),
                                                   ('buffer', True, 0, args.repeats),
                                                   ('single', True, 1, 1)]:
                entry[mode] = run(args.candidate, packet, args.output/(name+'-'+mode), repeats, workers, direct)
            def pixels(mode):
                with Image.open(args.output/(name+'-'+mode+'.png')) as image:
                    return np.array(image.convert('RGBA'), dtype=np.int16)
            owned = pixels('owned')
            assert np.array_equal(owned, pixels('buffer')), name+' buffer mismatch'
            assert np.array_equal(owned, pixels('single')), name+' worker mismatch'
            if args.baseline:
                entry['before'] = run(args.baseline, packet, args.output/(name+'-before'), args.repeats)
                difference = np.abs(owned-pixels('before'))
                entry['difference'] = dict(max_channel=int(difference.max()), mean=float(difference.mean()),
                                           changed_channels=int(np.count_nonzero(difference)))
                # FMA removal can move a rounding boundary by one byte; large
                # or widespread differences reject a speed-only optimization.
                assert difference.max() <= 1 and difference.mean() < 0.001, name+' appearance regression'
            report.append(entry)
            print(json.dumps(entry), flush=True)
    (args.output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
