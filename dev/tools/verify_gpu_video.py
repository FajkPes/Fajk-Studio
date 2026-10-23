"""Measure real moving video through the production R470 pipeline.

Alternate CPU/GPU runs with an identical total worker budget. The native helper
checks every exported packet against independent encoding, timestamps, repeated
decode, cross-backend decode, actual GPU dispatches and packet byte limits.
PNG/quality diagnostics run outside measured intervals. --full additionally
exports the complete source via the shipping CLI and decodes/cross-checks every
frame with bounded memory. Full export wall time includes process startup.
"""
import argparse
import csv
import ctypes
import hashlib
import json
import statistics
import subprocess
import time
from pathlib import Path


def run(command):
    start = time.perf_counter()
    process = subprocess.Popen(list(map(str, command)), stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    stdout, stderr = process.communicate()
    elapsed = (time.perf_counter() - start) * 1000
    if process.returncode:
        raise RuntimeError(stdout + stderr)
    # Windows keeps the process handle open after communicate(), so CPU work
    # includes all codec/driver threads without sampling a transient percentage.
    values = [ctypes.c_uint64() for _ in range(4)]
    get_times = ctypes.windll.kernel32.GetProcessTimes
    get_times.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_uint64)] * 4
    get_times.restype = ctypes.c_int
    if not get_times(int(process._handle), *map(ctypes.byref, values)):
        raise ctypes.WinError()
    return dict(stdout=stdout, wall_ms=elapsed,
                cpu_ms=(values[2].value + values[3].value) / 10000)


def rows(path):
    with path.open() as stream:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(stream)]


def main():
    dev = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path)
    p.add_argument('--build', type=Path, default=dev / 'build-r4')
    p.add_argument('--metric', type=Path, default=dev / 'build-r4/fajk_metric.exe')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--frames', type=int, default=32)
    p.add_argument('--workers', type=int, default=10)
    p.add_argument('--rounds', type=int, default=2)
    p.add_argument('--budgets', type=int, nargs='+', default=[30000, 80000, 150000])
    p.add_argument('--full', action='store_true')
    p.add_argument('--full-budget', type=int, default=80000)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    helper, cli = a.build / 'fajk_video_benchmark.exe', a.build / 'fajk_cli.exe'
    report = dict(method=__doc__, input=str(a.input.resolve()),
                  source_sha256=hashlib.sha256(a.input.read_bytes()).hexdigest(),
                  binaries={str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in [helper, cli]},
                  workers=a.workers, frames=a.frames, rounds=a.rounds, full_requested=a.full,
                  complete=False, cases=[], full={}, failures=[])

    def save():
        (a.output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    for budget in a.budgets:
        case = dict(budget=budget, runs=[], backends={})
        source_crcs = None
        packets = {}
        for round_id in range(a.rounds):
            for backend in (['cpu', 'gpu'] if round_id % 2 == 0 else ['gpu', 'cpu']):
                prefix = a.output / f'{budget}-{round_id}-{backend}'
                result = run([helper, a.input, prefix, a.frames, a.workers, budget, backend, 0])
                frames = rows(prefix.with_suffix('.csv'))
                summary = rows(Path(str(prefix) + '-summary.csv'))[0]
                crcs = [(r['source_crc'], r['pts'], r['duration']) for r in frames]
                if source_crcs is None:
                    source_crcs = crcs
                assert source_crcs == crcs, 'Source frames/timestamps differ between runs'
                packet_hash = hashlib.sha256(Path(str(prefix) + '-codec.fajkv').read_bytes()).hexdigest()
                if backend in packets:
                    assert packets[backend] == packet_hash, 'Non-deterministic video packets'
                packets[backend] = packet_hash
                assert len(frames) == a.frames and summary['unique_sources'] > 1
                assert all(r['bytes'] <= budget and r['gpu_fallbacks'] == 0 for r in frames)
                quality = []
                outliers = []
                for i in sorted({0, a.frames // 2, a.frames - 1} | {int(r['frame']) for r in frames if r['cross_max'] > 16}):
                    name = str(prefix) + f'-frame{i}'
                    scores = list(map(float, run([a.metric, name + '-source.png', name + '-decoded.png', name + '-cross.png'])['stdout'].split()))
                    if i in [0, a.frames // 2, a.frames - 1]:
                        quality.append(scores[0])
                    if frames[i]['cross_max'] > 16:
                        similarity = float(run([a.metric, name + '-decoded.png', name + '-cross.png'])['stdout'])
                        outliers.append(dict(frame=i, own_score=scores[0], cross_score=scores[1], similarity=similarity))
                        assert abs(scores[0] - scores[1]) <= .02, 'Cross-decode quality differs against the actual source'
                entry = dict(backend=backend, round=round_id, summary=summary,
                             encode_ms=statistics.median(r['encode_ms'] for r in frames),
                             decode_ms=statistics.median(r['decode_ms'] for r in frames),
                             encode_cpu_ms=statistics.mean(r['encode_cpu_ms'] for r in frames),
                             decode_cpu_ms=statistics.mean(r['decode_cpu_ms'] for r in frames),
                             quality=quality, local_differences=outliers, packet_sha256=packet_hash,
                             maximum_cross_error=max(r['cross_max'] for r in frames),
                             maximum_cross_mean=max(r['cross_mean'] for r in frames))
                case['runs'].append(entry)
                print(budget, round_id, backend, result['stdout'].strip(), flush=True)
        for backend in ['cpu', 'gpu']:
            runs = [r for r in case['runs'] if r['backend'] == backend]
            case['backends'][backend] = {k: statistics.median(r['summary'][k] for r in runs)
                                         for k in ['export_ms', 'export_cpu_ms', 'decode_ms', 'decode_cpu_ms']}
        for key in case['backends']['cpu']:
            case[key + '_ratio'] = case['backends']['gpu'][key] / case['backends']['cpu'][key]
        cpu_quality = next(r['quality'] for r in case['runs'] if r['backend'] == 'cpu')
        gpu_quality = next(r['quality'] for r in case['runs'] if r['backend'] == 'gpu')
        case['ssimulacra2_delta'] = [g - c for g, c in zip(gpu_quality, cpu_quality)]
        assert min(case['ssimulacra2_delta']) >= -.2, 'GPU encoding quality regression'
        report['cases'].append(case)
        save()
    if a.full:
        for backend in ['cpu', 'gpu']:
            prefix = a.output / f'full-{backend}'
            output = prefix.with_suffix('.fajkv')
            exported = run([cli, 'encode', a.input, output, '--quality', 100,
                            '--target-bytes', a.full_budget, '--workers', a.workers,
                            '--' + backend, '--overwrite'])
            print('full', backend, 'export', round(exported['wall_ms']), 'ms', flush=True)
            checked = run([helper, '--decode', output, prefix, a.workers, backend, a.input])
            summary = rows(Path(str(prefix) + '-summary.csv'))[0]
            outliers = []
            for frame in rows(prefix.with_suffix('.csv')):
                if frame['cross_max'] > 16:
                    name = str(prefix) + f"-frame{int(frame['frame'])}"
                    similarity = float(run([a.metric, name + '.png', name + '-cross.png'])['stdout'])
                    scores = list(map(float, run([a.metric, name + '-source.png', name + '.png', name + '-cross.png'])['stdout'].split()))
                    outliers.append(dict(frame=int(frame['frame']), maximum=frame['cross_max'], similarity=similarity, own_score=scores[0], cross_score=scores[1]))
                    assert abs(scores[0] - scores[1]) <= .02, 'Full-video quality differs against the actual source'
            report['full'][backend] = dict(export=exported, decode=summary,
                                           validation=checked['stdout'], local_differences=outliers)
            save()
            print('full', backend, checked['stdout'].strip(), flush=True)
        assert report['full']['cpu']['decode']['frames'] == report['full']['gpu']['decode']['frames']
        assert report['full']['cpu']['decode']['duration'] == report['full']['gpu']['decode']['duration']
    report['complete'] = True
    save()


if __name__ == '__main__':
    main()
