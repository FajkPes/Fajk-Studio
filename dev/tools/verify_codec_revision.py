"""Alternate saved and candidate codec binaries with identical settings.

Compare full packets and decoded RGBA exactly, and retain per-frame timings.
Use this after a performance-only change; CPU/GPU are compared with their own
saved backend, so the existing differences between backends do not hide changes.
Needs Pillow. Startup, two warmups per process, and PNG IO are not timed.
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
    p.add_argument('--inputs', type=Path, nargs='+', default=[dev/'samples/references'/f'{n}-reference.png' for n in ['22920', '22950', '23006']])
    p.add_argument('--budgets', type=int, nargs='+', default=[30000, 80000, 150000])
    p.add_argument('--backends', choices=['cpu', 'gpu'], nargs='+', default=['cpu', 'gpu'])
    p.add_argument('--repeats', type=int, default=8)
    p.add_argument('--rounds', type=int, default=2)
    p.add_argument('--workers', type=int, default=10)
    p.add_argument('--reuse', action='store_true', help='Resume complete saved runs, requiring the same binary hashes.')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    binaries = {'reference': a.reference.resolve(), 'candidate': a.candidate.resolve()}
    report = dict(method=__doc__, binaries={key: dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for key, path in binaries.items()},
                  repeats=a.repeats, rounds=a.rounds, workers=a.workers, cases=[], failures=[])
    previous = a.output/'results.json'
    if a.reuse and previous.exists():
        saved = json.loads(previous.read_text(encoding='utf-8'))
        for variant in binaries:
            if saved['binaries'][variant]['sha256'] != report['binaries'][variant]['sha256']:
                raise RuntimeError('Cannot reuse measurements from different binaries.')
        if any(saved[key] != report[key] for key in ['repeats', 'rounds', 'workers']):
            raise RuntimeError('Cannot reuse measurements with different benchmark settings.')
    for source in a.inputs:
        for budget in a.budgets:
            for backend in a.backends:
                tag = f'{source.stem}-{budget}-{backend}'
                rows = {key: [] for key in binaries}
                last = {}
                for round_id in range(a.rounds):
                    order = ['reference', 'candidate'] if round_id % 2 == 0 else ['candidate', 'reference']
                    for variant in order:
                        prefix = (a.output/f'{tag}-{round_id}-{variant}').resolve()
                        command = [binaries[variant], 'measure', source.resolve(), prefix, 100, a.repeats, 420, a.workers, 100, 'wavelet', budget-168, 0, 'r470-buffer', 50, 1, 0, 0, f'--{backend}']
                        complete = False
                        if a.reuse and all(prefix.with_suffix(ext).exists() for ext in ['.csv', '.png', '.fajkv']):
                            with prefix.with_suffix('.csv').open() as stream:
                                complete = len(list(csv.DictReader(stream))) == a.repeats
                        if not complete:
                            result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
                            if result.returncode:
                                raise RuntimeError(result.stdout+result.stderr)
                        with prefix.with_suffix('.csv').open() as stream:
                            rows[variant].extend(csv.DictReader(stream))
                        last[variant] = prefix
                case = dict(source=str(source), budget=budget, backend=backend, variants={})
                for variant, samples in rows.items():
                    case['variants'][variant] = {key: statistics.median(float(row[key]) for row in samples) for key in ['encode_ms', 'decode_ms']}
                    case['variants'][variant]['gpu_fallbacks'] = sum(int(row['gpu_fallbacks']) for row in samples)
                    case['variants'][variant]['gpu_contour_filters'] = sum(int(row.get('gpu_contour_filters', 0)) for row in samples)
                    if case['variants'][variant]['gpu_fallbacks']:
                        report['failures'].append(tag+': GPU error fallback '+variant)
                    for key in ['encode_cpu_ms', 'decode_cpu_ms']:
                        case['variants'][variant][key] = statistics.mean(float(row[key]) for row in samples)
                    if any(int(row['bytes'])+168 > budget for row in samples):
                        report['failures'].append(tag+': budget exceeded '+variant)
                x, y = last['reference'], last['candidate']
                case['exact_packet'] = x.with_suffix('.fajkv').read_bytes() == y.with_suffix('.fajkv').read_bytes()
                with Image.open(x.with_suffix('.png')) as image:
                    pixels = image.convert('RGBA').tobytes()
                with Image.open(y.with_suffix('.png')) as image:
                    case['exact_pixels'] = pixels == image.convert('RGBA').tobytes()
                if not case['exact_packet'] or not case['exact_pixels']:
                    report['failures'].append(tag+': performance change altered packet or pixels')
                for key in ['encode_ms', 'decode_ms', 'encode_cpu_ms', 'decode_cpu_ms']:
                    baseline = case['variants']['reference'][key]
                    case[key+'_ratio'] = case['variants']['candidate'][key]/baseline if baseline else None
                report['cases'].append(case)
                (a.output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
                print(tag, 'encode', round(case['encode_ms_ratio'], 3), 'decode', round(case['decode_ms_ratio'], 3),
                      'exact', case['exact_packet'] and case['exact_pixels'], flush=True)
    if report['failures']:
        raise SystemExit('\n'.join(report['failures']))


if __name__ == '__main__':
    main()
