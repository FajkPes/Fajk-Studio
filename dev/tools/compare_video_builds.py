"""Compare production video pipelines in two builds, requiring exact packets/frames.

Each native helper also checks timestamps, independent encode, cancellation,
memory guards, repeated decode and CPU/GPU cross-decode. Run without other
benchmarks/builds. Wall time includes transfers and waits; PNG checks are untimed.
"""
import argparse
import csv
import hashlib
import json
import statistics
import subprocess
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ['before', 'after', 'input', 'output']:
        p.add_argument('--' + key, type=Path, required=True)
    p.add_argument('--frames', type=int, default=24)
    p.add_argument('--workers', type=int, default=10)
    p.add_argument('--budget', type=int, default=80000)
    p.add_argument('--rounds', type=int, default=4)
    a = p.parse_args()
    if a.rounds < 2:
        p.error('Use at least two rounds.')
    a.output.mkdir(parents=True, exist_ok=True)
    sha = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
    report = dict(method=__doc__, before_sha256=sha(a.before), after_sha256=sha(a.after),
                  source_sha256=sha(a.input), frames=a.frames, budget=a.budget,
                  workers=a.workers, records=[], failures=[], complete=False)
    signatures = {}
    for turn in range(a.rounds):
        variants = [(label, backend) for label in ['before', 'after'] for backend in ['cpu', 'gpu']]
        # Rotate and reverse, avoiding always testing the same backend/build last.
        variants = variants[turn % 4:] + variants[:turn % 4]
        if turn % 2:
            variants.reverse()
        for label, backend in variants:
            prefix = a.output / f'{turn}-{label}-{backend}'
            result = subprocess.run(list(map(str, [getattr(a, label), a.input, prefix,
                a.frames, a.workers, a.budget, backend, 0])), capture_output=True, text=True)
            (Path(str(prefix) + '.log')).write_text(result.stdout+result.stderr, encoding='utf-8')
            if result.returncode:
                raise RuntimeError(result.stdout+result.stderr)
            with Path(str(prefix)+'-summary.csv').open() as stream:
                summary = {k: float(v) for k, v in next(csv.DictReader(stream)).items()}
            with prefix.with_suffix('.csv').open() as stream:
                rows = list(csv.DictReader(stream))
            signature = (sha(Path(str(prefix)+'-pipeline.fajkv')),
                         tuple((r['pts'], r['duration'], r['decoded_crc']) for r in rows))
            if backend in signatures and signatures[backend] != signature:
                report['failures'].append(f'{turn}/{label}/{backend}: packet, pixels or timing changed')
            signatures[backend] = signature
            report['records'].append(dict(round=turn, label=label, backend=backend,
                                          packet_sha256=signature[0], summary=summary))
            (a.output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            print(turn, label, backend, 'export', summary['export_ms'],
                  'decode', summary['decode_ms'], flush=True)
    report['summary'] = {}
    for backend in ['cpu', 'gpu']:
        values = {}
        for label in ['before', 'after']:
            selected = [r['summary'] for r in report['records'] if r['label']==label and r['backend']==backend]
            values[label] = {k: statistics.median(r[k] for r in selected)
                             for k in ['export_ms','export_cpu_ms','decode_ms','decode_cpu_ms']}
        values['ratios'] = {k: values['after'][k]/values['before'][k] for k in values['before']}
        report['summary'][backend] = values
    report['complete'] = True
    (a.output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if report['failures']:
        raise SystemExit('\n'.join(report['failures']))


if __name__ == '__main__':
    main()
