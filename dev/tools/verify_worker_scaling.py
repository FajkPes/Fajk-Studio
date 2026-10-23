"""Alternate worker budgets on one identical image; require exact codec output.

This tests throughput/CPU work rather than assuming that more threads are
faster. Runs are serial, orders reversed on the second round, warmups and PNG
IO excluded by the CLI. CPU and GPU are compared to their own 10-worker output.
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
    p.add_argument('--cli', type=Path, default=dev / 'build-r4/fajk_cli.exe')
    p.add_argument('--input', type=Path, default=dev / 'samples/references/22950-reference.png')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--workers', type=int, nargs='+', default=[6, 8, 10, 12])
    p.add_argument('--repeats', type=int, default=16)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    records = []
    for round_id in range(2):
        budgets = a.workers if round_id == 0 else list(reversed(a.workers))
        for workers in budgets:
            for backend in (['cpu', 'gpu'] if round_id == 0 else ['gpu', 'cpu']):
                prefix = a.output / f'{workers}-{round_id}-{backend}'
                command = [a.cli, 'measure', a.input, prefix, 100, a.repeats, 420, workers, 100,
                           'wavelet', 80000-168, 0, 'r470-buffer', 50, 1, 0, 0, '--' + backend]
                subprocess.run(list(map(str, command)), check=True, capture_output=True)
                with prefix.with_suffix('.csv').open() as stream:
                    samples = list(csv.DictReader(stream))
                entry = dict(workers=workers, backend=backend, round=round_id,
                             packet=hashlib.sha256(prefix.with_suffix('.fajkv').read_bytes()).hexdigest(),
                             pixels=hashlib.sha256(Image.open(prefix.with_suffix('.png')).convert('RGBA').tobytes()).hexdigest(),
                             **{k: statistics.median(float(r[k]) for r in samples) for k in ['encode_ms', 'decode_ms']},
                             **{k: statistics.mean(float(r[k]) for r in samples) for k in ['encode_cpu_ms', 'decode_cpu_ms']})
                assert all(int(r['gpu_fallbacks']) == 0 for r in samples)
                records.append(entry)
                print(workers, backend, 'encode', round(entry['encode_ms'], 3), 'decode', round(entry['decode_ms'], 3), flush=True)
    for backend in ['cpu', 'gpu']:
        selected = [r for r in records if r['backend'] == backend]
        assert len({r['packet'] for r in selected}) == len({r['pixels'] for r in selected}) == 1, 'Worker budget changed output'
    summary = []
    for backend in ['cpu', 'gpu']:
        for workers in a.workers:
            selected = [r for r in records if r['backend'] == backend and r['workers'] == workers]
            summary.append(dict(backend=backend, workers=workers, **{k: statistics.median(r[k] for r in selected)
                           for k in ['encode_ms', 'decode_ms', 'encode_cpu_ms', 'decode_cpu_ms']}))
    report = dict(method=__doc__, cli_sha256=hashlib.sha256(a.cli.read_bytes()).hexdigest(),
                  input_sha256=hashlib.sha256(a.input.read_bytes()).hexdigest(), records=records, summary=summary)
    (a.output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
