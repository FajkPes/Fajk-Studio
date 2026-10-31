"""Check each exported Max strength against a chosen previous export.

All frames get source MSE and stationary/motion-aligned error-change metrics.
SSIMULACRA2 uses only six explicitly listed snapshots, never the whole clip.
Optional SDK validation demands exact old/new decoder pixels on the same new
file, separately for CPU and GPU; it does not demand equality between encoders.
"""
import argparse
import csv
import hashlib
import json
import math
import statistics
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('probe', 'metric', 'video', 'exports', 'baseline-directory', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--library', type=Path)
    parser.add_argument('--old-library', type=Path)
    parser.add_argument('--case', action='append', help='Limit to gpu:80000:6, etc.')
    parser.add_argument('--skip-perceptual', action='store_true', help='Defer the six expensive snapshot scores')
    parser.add_argument('--reuse-probe', action='store_true', help='Reuse a completed probe with exactly matching input hashes')
    args = parser.parse_args()
    assert bool(args.library) == bool(args.old_library)
    exports = json.loads(args.exports.read_text())
    assert exports['complete']
    args.output.mkdir(parents=True, exist_ok=True)
    summary = dict(complete=False, records=[])

    def save():
        (args.output / 'results.json').write_text(json.dumps(summary, indent=2))

    save()
    for export in exports['records']:
        backend, budget, strength = export['backend'], export['budget'], export['strength']
        if args.case and f'{backend}:{budget}:{strength}' not in args.case:
            continue
        name = f'{backend}-{budget}-max{strength}'
        output = args.output / name
        output.mkdir(parents=True, exist_ok=True)
        video = Path(export['path'])
        assert hashlib.sha256(video.read_bytes()).hexdigest() == export['sha256']
        baseline = args.baseline_directory / f'{backend}-{budget}.fajkv'
        identity = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in
                    [('probe', args.probe), ('source', args.video), ('baseline', baseline), ('candidate', video)]}
        identity['frames'] = exports['frames']
        previous = output / 'probe-identity.json'
        reusable = args.reuse_probe and previous.exists() and json.loads(previous.read_text()) == identity
        if not reusable:
            command = [args.probe, args.video, baseline, video, output, exports['frames'],
                       1 if backend == 'gpu' else 0, 1, 1]
            with (output / 'probe.log').open('wb') as log:
                subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
            previous.write_text(json.dumps(identity, indent=2))
        assert f"PASS {exports['frames']} frames" in (output / 'probe.log').read_text()
        rows = list(csv.DictReader((output / 'quality.csv').open()))
        assert len(rows) == exports['frames']
        row = dict(backend=backend, budget=budget, strength=strength, frames=len(rows),
                   sha256=export['sha256'], baseline=str(baseline), quality={}, selected_ssimulacra2=[])
        summary['records'].append(row)
        for before, after in [('mse_intra', 'mse_max'), ('flicker_intra', 'flicker_max'),
                              ('motion_flicker_intra', 'motion_flicker_max')]:
            if before.startswith('mse'):
                values = [statistics.mean(float(r[key]) for r in rows) for key in (before, after)]
            else:
                weight = 'motion_stable_samples' if before.startswith('motion') else 'stable_samples'
                total = sum(int(r[weight]) for r in rows)
                assert total > 0
                values = [math.sqrt(sum(float(r[key])**2*int(r[weight]) for r in rows)/total)
                          for key in (before, after)]
            row['quality'][before.replace('_intra', '')] = dict(before=values[0], after=values[1],
                change_percent=100*(values[1]/values[0]-1))
        save()
        frames = [] if args.skip_perceptual else [1, 12, 23, 47, 119, exports['frames']-1]
        for frame in frames:
            result = subprocess.run([str(args.metric), str(output / f'{frame}-source.png'),
                str(output / f'{frame}-intra.png'), str(output / f'{frame}-max.png')],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, text=True)
            scores = [float(x) for x in result.stdout.split()]
            assert len(scores) == 2, result.stdout
            row['selected_ssimulacra2'].append(dict(frame=frame, before=scores[0], after=scores[1]))
        row['mean_selected_ssimulacra2'] = ({name: statistics.mean(r[name] for r in row['selected_ssimulacra2'])
                                           for name in ('before', 'after')} if frames else None)
        save()
        print('PASS quality', name, row['quality'], row['mean_selected_ssimulacra2'], flush=True)
        if args.library:
            command = [sys.executable, str(Path(__file__).with_name('compare_motion_decoders.py')),
                '--before', str(args.old_library), '--after', str(args.library),
                '--before-video', str(video), '--after-video', str(video),
                '--output', str(output / 'compatibility.json'), '--repeats', '1']
            subprocess.run(command, check=True)
            row['compatibility'] = json.loads((output / 'compatibility.json').read_text())
            assert row['compatibility']['complete']
            save()
    summary['complete'] = True
    save()


if __name__ == '__main__':
    main()
