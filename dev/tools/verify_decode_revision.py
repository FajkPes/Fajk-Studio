"""Compare saved/candidate decoders on identical packets and worker budgets.

Alternates process order, excludes two warmups/PNG IO, and includes all GPU
transfers in wall time and process CPU work. Retains profiles and exact RGBA.
Needs Pillow. No encoder work runs during these measurements.
"""
import argparse
import csv
import hashlib
import json
import statistics
import subprocess
from pathlib import Path
from PIL import Image


def main():
    dev = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--candidate', type=Path, default=dev/'build-r4/fajk_cli.exe')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--inputs', type=Path, nargs='+', required=True)
    p.add_argument('--workers', type=int, nargs='+', default=[1, 2, 4, 10])
    p.add_argument('--backend', choices=['cpu', 'gpu'], default='gpu')
    p.add_argument('--repeats', type=int, default=32)
    p.add_argument('--rounds', type=int, default=2)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    binaries = {'reference': a.reference.resolve(), 'candidate': a.candidate.resolve()}
    report = dict(method=__doc__, backend=a.backend, repeats=a.repeats, rounds=a.rounds,
                  binaries={k: dict(path=str(v), sha256=hashlib.sha256(v.read_bytes()).hexdigest()) for k, v in binaries.items()},
                  cases=[], failures=[])
    for packet in a.inputs:
        for workers in a.workers:
            tag = f'{packet.stem}-w{workers}'
            samples = {k: [] for k in binaries}
            last = {}
            for round_id in range(a.rounds):
                order = ['reference', 'candidate'] if round_id % 2 == 0 else ['candidate', 'reference']
                for variant in order:
                    prefix = (a.output/f'{tag}-{round_id}-{variant}').resolve()
                    command = [binaries[variant], 'measure-decode', packet.resolve(), prefix, a.repeats, workers, 'buffer', f'--{a.backend}']
                    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
                    if result.returncode:
                        raise RuntimeError(result.stdout+result.stderr)
                    with prefix.with_suffix('.csv').open() as stream:
                        samples[variant].extend(csv.DictReader(stream))
                    last[variant] = prefix
            case = dict(packet=str(packet), packet_sha256=hashlib.sha256(packet.read_bytes()).hexdigest(), workers=workers, variants={})
            for variant, rows in samples.items():
                case['variants'][variant] = dict(decode_ms=statistics.median(float(r['decode_ms']) for r in rows),
                                                cpu_ms=statistics.mean(float(r['cpu_ms']) for r in rows),
                                                cpu_cycles=statistics.mean(int(r['cpu_cycles']) for r in rows),
                                                gpu_contour_filters=sum(int(r.get('gpu_contour_filters', 0)) for r in rows),
                                                gpu_fallbacks=sum(int(r['gpu_fallbacks']) for r in rows))
                if any(int(r['gpu_fallbacks']) for r in rows):
                    report['failures'].append(tag+': GPU error fallback '+variant)
            with Image.open(last['reference'].with_suffix('.png')) as image:
                pixels = image.convert('RGBA').tobytes()
            with Image.open(last['candidate'].with_suffix('.png')) as image:
                case['exact_pixels'] = pixels == image.convert('RGBA').tobytes()
            if not case['exact_pixels']:
                report['failures'].append(tag+': output changed')
            for key in ['decode_ms', 'cpu_ms', 'cpu_cycles']:
                reference = case['variants']['reference'][key]
                case[key+'_ratio'] = case['variants']['candidate'][key]/reference if reference else None
            report['cases'].append(case)
            (a.output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            print(tag, 'decode', round(case['decode_ms_ratio'], 3), 'CPU work', round(case['cpu_ms_ratio'], 3), 'exact', case['exact_pixels'], flush=True)
    if report['failures']:
        raise SystemExit('\n'.join(report['failures']))


if __name__ == '__main__':
    main()
