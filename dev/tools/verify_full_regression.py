"""Check a new GPU video export and both decoders against complete saved videos.

The reference directory must contain full-cpu/full-gpu.fajkv and their per-frame
CSVs from verify_gpu_video.py. CRC comparison is regression checking, not a
claim that CPU and GPU floating-point results match each other.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
from verify_gpu_video import run


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['input', 'baseline', 'build', 'output']:
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--workers', type=int, default=10)
    p.add_argument('--budget', type=int, default=80000)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    cli, helper = a.build/'fajk_cli.exe', a.build/'fajk_video_benchmark.exe'
    sha = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
    report = dict(method=__doc__, cli_sha256=sha(cli), helper_sha256=sha(helper),
                  source_sha256=sha(a.input), workers=a.workers, budget=a.budget,
                  decoders={}, complete=False)
    export = a.output/'full-gpu.fajkv'
    report['gpu_export'] = run([cli, 'encode', a.input, export, '--quality', 100,
                                '--target-bytes', a.budget, '--workers', a.workers,
                                '--gpu', '--overwrite'])
    report['gpu_packet_sha256'] = sha(export)
    report['gpu_packet_exact'] = sha(a.baseline/'full-gpu.fajkv') == report['gpu_packet_sha256']
    assert report['gpu_packet_exact'], 'GPU export changed the reference bitstream'
    print('GPU full export', round(report['gpu_export']['wall_ms']), 'ms; exact entire reference file', flush=True)
    for backend in ['cpu', 'gpu']:
        packet = a.baseline/'full-cpu.fajkv' if backend=='cpu' else export
        prefix = a.output/f'full-{backend}'
        result = run([helper, '--decode', packet, prefix, a.workers, backend, a.input])
        with Path(str(prefix)+'-summary.csv').open() as stream:
            summary = {k: float(v) for k, v in next(csv.DictReader(stream)).items()}
        with prefix.with_suffix('.csv').open() as stream:
            actual = list(csv.DictReader(stream))
        with (a.baseline/f'full-{backend}.csv').open() as stream:
            reference = list(csv.DictReader(stream))
        assert len(actual)==len(reference)==int(summary['frames'])
        exact = all(tuple(x[k] for k in ['pts','duration','decoded_crc']) ==
                    tuple(y[k] for k in ['pts','duration','decoded_crc']) for x,y in zip(actual,reference))
        assert exact, 'A decoded frame or timestamp changed'
        assert not summary['gpu_fallbacks']
        if backend=='gpu':
            assert summary['gpu_decodes']==summary['frames']
        report['decoders'][backend] = dict(summary=summary, all_frame_crc_and_timestamps_exact=exact,
                                           validation=result['stdout'])
        (a.output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(backend, result['stdout'].strip(), 'all reference CRCs unchanged', flush=True)
    report['complete']=True
    (a.output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__=='__main__':
    main()
