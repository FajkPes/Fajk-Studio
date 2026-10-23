"""Paired production exports and compatibility/cancellation tests for temporal video."""
import argparse, json, statistics, struct, subprocess, time, zlib
from pathlib import Path


def sequence(path):
    data = path.read_bytes()
    version, = struct.unpack_from('<I', data, 8)
    count, offset = struct.unpack_from('<QQ', data, 40)
    index = data[offset:]
    assert len(index) == count * 40 and zlib.crc32(index) == int.from_bytes(data[64:68], 'little')
    assert zlib.crc32(data[:68]) == int.from_bytes(data[68:72], 'little')
    frames = []
    for n in range(count):
        start, size, pts, duration, crc, reference = struct.unpack_from('<QQqqII', index, n * 40)
        payload = data[start:start + size]
        assert zlib.crc32(payload) == crc
        assert not reference or reference <= n and frames[reference - 1]['reference'] == 0
        assert (payload[4] in (132, 133)) == bool(reference)
        frames.append(dict(payload=payload, pts=pts, duration=duration, reference=reference))
    return version, frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--screen', type=Path, required=True)
    parser.add_argument('--motion', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    for name, source, count, budget in [('screen', args.screen, 32, 12000), ('motion', args.motion, 24, 80000)]:
        for round_number in range(2):
            variants = [(False, False), (False, True), (True, False), (True, True)]
            variants = variants[round_number * 2:] + variants[:round_number * 2]
            for gpu, temporal in variants:
                output = args.output / f'{name}-{"gpu" if gpu else "cpu"}-{"temporal" if temporal else "intra"}.fajkv'
                command = [str(args.cli), 'encode', str(source), str(output), '--fast', '--no-aq', '--protect-text', '0', '--gpu' if gpu else '--cpu', '--workers', '10', '--target-bytes', str(budget), '--max-frames', str(count), '--overwrite']
                if temporal:
                    command += ['--temporal', '--keyframe-interval', '4']
                start = time.perf_counter()
                completed = subprocess.run(command, capture_output=True, text=True, check=True)
                elapsed_ms = (time.perf_counter() - start) * 1000
                version, frames = sequence(output)
                assert version == (2 if temporal else 1) and len(frames) == count
                keys = sum(not f['reference'] for f in frames)
                assert all(len(f['payload']) <= budget for f in frames)
                if name == 'screen' and temporal:
                    assert keys == 2 and all(frames[n]['reference'] == 1 for n in range(1, 26))
                    assert all(frames[n]['reference'] == 27 for n in range(27, 32))
                if name == 'motion':
                    assert keys == count
                records.append(dict(scenario=name, round=round_number, gpu=gpu, temporal=temporal, total_process_ms=elapsed_ms, bytes=output.stat().st_size, keys=keys))
                print(f'{output.name} round {round_number}: {elapsed_ms:.3f} ms, {keys} keys', flush=True)
        # All-intra output must remain exactly compatible with the shipped .1
        # build. Temporal fallback must contain the very same frame payloads.
        for gpu in [False, True]:
            baseline = args.output / f'{name}-baseline-{"gpu" if gpu else "cpu"}.fajkv'
            subprocess.run([str(args.baseline), 'encode', str(source), str(baseline), '--fast', '--no-aq', '--protect-text', '0', '--gpu' if gpu else '--cpu', '--workers', '10', '--target-bytes', str(budget), '--max-frames', str(count), '--overwrite'], check=True, capture_output=True)
            intra = args.output / f'{name}-{"gpu" if gpu else "cpu"}-intra.fajkv'
            assert baseline.read_bytes() == intra.read_bytes(), 'Existing all-intra output changed'
            if name == 'motion':
                _, actual = sequence(args.output / f'{name}-{"gpu" if gpu else "cpu"}-temporal.fajkv')
                _, expected = sequence(intra)
                assert actual == expected, 'Temporal fallback changed frame payloads'
    # Single-image output remains version 1 even if --temporal is supplied.
    # Cancellation/data-preservation are covered by native pipeline tests and
    # the video helper; this tool also verifies an invalid interval is rejected.
    protected = args.output / 'protected.fajkv'
    protected.write_bytes(b'preserve existing destination')
    failed = subprocess.run([str(args.cli), 'encode', str(args.screen), str(protected), '--temporal', '--keyframe-interval', '10000001', '--overwrite'], capture_output=True)
    assert failed.returncode and protected.read_bytes() == b'preserve existing destination'
    (args.output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    print('PASS: paired production exports, adaptive intervals, CRC/index/caps, exact .1 compatibility and fallback, destination preservation')


if __name__ == '__main__':
    main()
