"""Alternate baseline/candidate real exports; validate budgets and determinism.

Use --require-identical for optimizations that must preserve every packet.
Quality-changing experiments also need motion_quality_probe; size alone does
not establish a useful improvement. Timings include input decode and file IO.
"""
import argparse
import hashlib
import json
import statistics
from pathlib import Path
from verify_coefficient_temporal import inspect, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['baseline-cli', 'candidate-cli', 'video', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--frames', type=int, default=240)
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--budgets', type=int, nargs='+', default=[80000, 30000])
    parser.add_argument('--backends', choices=['gpu', 'cpu'], nargs='+', default=['gpu'])
    parser.add_argument('--tolerance', type=int, default=6)
    parser.add_argument('--require-identical', action='store_true')
    args = parser.parse_args()
    assert args.frames > 1 and args.repeats > 0
    args.output.mkdir(parents=True, exist_ok=True)
    records = []

    def save(complete=False):
        (args.output / 'results.json').write_text(json.dumps(dict(
            complete=complete, frames=args.frames, repeats=args.repeats,
            tolerance=args.tolerance, records=records), indent=2))

    for backend in args.backends:
        for budget in args.budgets:
            record = dict(backend=backend, budget=budget)
            records.append(record)
            for name in ['baseline', 'candidate']:
                record[name] = dict(path=str(args.output / f'{backend}-{budget}-{name}.fajkv'),
                                    measurements=[], sha256=None)
            for repeat in range(args.repeats):
                order = ['baseline', 'candidate'] if repeat % 2 == 0 else ['candidate', 'baseline']
                for name in order:
                    item = record[name]
                    cli = args.baseline_cli if name == 'baseline' else args.candidate_cli
                    command = [cli, 'encode', args.video, item['path'], '--fast', '--no-aq',
                        '--protect-text', 0, '--' + backend, '--workers', 10, '--420',
                        '--target-bytes', budget, '--max-frames', args.frames, '--overwrite',
                        '--temporal-max', '--similar-tolerance', args.tolerance]
                    measurement = run(command)
                    digest = hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()
                    assert item['sha256'] in [None, digest], (name, 'nondeterministic export')
                    item['sha256'] = digest
                    item['measurements'].append(measurement)
                    print(backend, budget, name, repeat, measurement, flush=True)
                    save()
            for name in ['baseline', 'candidate']:
                item = record[name]
                packets = inspect(Path(item['path']))
                assert len(packets) == args.frames and all(p['size'] <= budget for p in packets)
                assert max(p['depth'] for p in packets) <= 16
                item.update(bytes=Path(item['path']).stat().st_size,
                    keyframes=sum(not p['reference'] for p in packets),
                    max_depth=max(p['depth'] for p in packets),
                    median_ms=statistics.median(m['wall_ms'] for m in item['measurements']))
            record['identical'] = record['baseline']['sha256'] == record['candidate']['sha256']
            if args.require_identical:
                assert record['identical'], 'Optimization changed the encoded file'
            record['speedup'] = record['baseline']['median_ms'] / record['candidate']['median_ms']
            record['byte_saving_percent'] = 100 * (1 - record['candidate']['bytes'] / record['baseline']['bytes'])
            print('PASS', backend, budget, record['speedup'], 'x; identical:', record['identical'], flush=True)
            save()
    save(True)


if __name__ == '__main__':
    main()
