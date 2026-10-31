"""Export Max strengths and inspect compression without asserting speed gains.

Useful while other applications are active: sizes, CRC/index validation and
reference limits are meaningful; wall-clock comparisons under load are not.
Run motion_quality_probe separately before accepting a lossy strength change.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from verify_coefficient_temporal import inspect


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('cli', 'video', 'intra-reference', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--case', action='append', required=True, help='gpu:80000:6 or cpu:30000:12')
    parser.add_argument('--frames', type=int, default=240)
    parser.add_argument('--external-load', default='Not measured; speed is not assessed by this tool')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    reference = json.loads(args.intra_reference.read_text())
    anchors = {(r['backend'], r['budget']): r for r in reference['records'] if r['tolerance'] is None}
    summary = dict(complete=False, frames=args.frames, timing_valid=False,
                   external_load=args.external_load, records=[])

    def save():
        (args.output / 'results.json').write_text(json.dumps(summary, indent=2))

    save()
    for case in args.case:
        backend, budget, strength = case.split(':')
        budget, strength = int(budget), int(strength)
        assert backend in ('gpu', 'cpu') and budget > 0 and 0 <= strength <= 16
        output = args.output / f'{backend}-{budget}-max{strength}.fajkv'
        command = [args.cli, 'encode', args.video, output, '--fast', '--no-aq', '--protect-text', 0,
            '--' + backend, '--workers', 10, '--420', '--target-bytes', budget,
            '--max-frames', args.frames, '--overwrite', '--temporal-max', '--similar-tolerance', strength]
        with output.with_suffix('.log').open('wb') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
        packets = inspect(output)
        assert len(packets) == args.frames and all(p['size'] <= budget for p in packets)
        assert max(p['depth'] for p in packets) <= 16
        anchor = anchors[1 if backend == 'gpu' else 0, budget]
        assert reference['frames'] == args.frames
        size = output.stat().st_size
        row = dict(backend=backend, budget=budget, strength=strength, path=str(output), bytes=size,
            sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
            keyframes=sum(not p['reference'] for p in packets), max_depth=max(p['depth'] for p in packets),
            intra_bytes=anchor['bytes'], saving_vs_intra_percent=100*(1-size/anchor['bytes']))
        summary['records'].append(row)
        save()
        print('PASS', json.dumps(row), flush=True)
    summary['complete'] = True
    save()


if __name__ == '__main__':
    main()
