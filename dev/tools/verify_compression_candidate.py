"""Validate the final binary against archived paired compression measurements.

Re-export each measured setting and demand the same complete file, then compare
old/new SDK pixels, timestamps and guarded output on sequential/random access.
Run after native tests; concurrent workloads invalidate any timings collected.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from verify_coefficient_temporal import inspect, run
from verify_motion_temporal import guarded_decode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('cli', 'library', 'old-library', 'video', 'output'):
        parser.add_argument('--' + flag, type=Path, required=True)
    parser.add_argument('--measurements', type=Path, nargs='+', required=True)
    parser.add_argument('--hold', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = dict(complete=False, exports=[], hold=None)

    def save():
        (args.output / 'validation.json').write_text(json.dumps(result, indent=2))

    for measurement in args.measurements:
        measured = json.loads(measurement.read_text())
        assert measured['complete']
        for item in measured['records']:
            backend, budget = item['backend'], item['budget']
            output = args.output / f'{backend}-{budget}.fajkv'
            timing = run([args.cli, 'encode', args.video, output, '--fast', '--no-aq',
                '--protect-text', 0, '--' + backend, '--workers', 10, '--420',
                '--target-bytes', budget, '--max-frames', measured['frames'],
                '--overwrite', '--temporal-max', '--similar-tolerance', measured['tolerance']])
            digest = hashlib.sha256(output.read_bytes()).hexdigest()
            assert digest == item['candidate']['sha256'], 'Final binary changed measured output'
            packets = inspect(output)
            assert len(packets) == measured['frames']
            assert all(p['size'] <= budget for p in packets)
            assert max(p['depth'] for p in packets) <= 16
            row = dict(backend=backend, budget=budget, sha256=digest, bytes=output.stat().st_size,
                       export=timing, sdk=[])
            result['exports'].append(row)
            save()
            compatibility = args.output / f'compatibility-{backend}-{budget}.json'
            subprocess.run([sys.executable, str(Path(__file__).with_name('compare_motion_decoders.py')),
                '--before', str(args.old_library), '--after', str(args.library),
                '--before-video', str(output), '--after-video', str(output),
                '--output', str(compatibility), '--repeats', '1'], check=True)
            row['compatibility'] = json.loads(compatibility.read_text())
            for decode_backend in (0, 1):
                row['sdk'].append(guarded_decode(args.library, output, decode_backend))
            print('PASS final export and SDK', backend, budget, digest, flush=True)
            save()
    if args.hold:
        output = args.output / 'long-hold.fajkv'
        run([args.cli, 'encode', args.hold, output, '--fast', '--no-aq', '--protect-text', 0,
             '--gpu', '--workers', 10, '--420', '--target-bytes', 4000, '--max-frames', 420, '--overwrite',
             '--temporal-max', '--similar-tolerance', 6])
        packets = inspect(output)
        # The fixture changes once, at 12 s. An unchanged image must survive
        # the 5 s refresh limit; the first changed frame must refresh the key.
        assert len(packets) == 420
        assert [n for n, p in enumerate(packets) if not p['reference']] == [0, 360]
        assert all(packets[n]['size'] == 8 for n in list(range(2, 360)) + list(range(361, 420)))
        result['hold'] = dict(frames=len(packets), keyframes=2, bytes=output.stat().st_size,
            sdk=[guarded_decode(args.library, output, backend) for backend in (0, 1)])
        print('PASS static segments, keyframes only at start and actual change', len(packets), flush=True)
    result['complete'] = True
    save()


if __name__ == '__main__':
    main()
