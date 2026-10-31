"""Real Max-compression exports, deterministic packets, guarded SDK and seeks.

Export timings include input decoding, process startup and atomic file commit.
They are collected before any SDK/quality probes, without concurrent workloads.
Lossy Max output is compared to itself on fresh/random access, never asserted
pixel-identical to an intra encode. Use motion_quality_probe for source quality.
"""
import argparse
import ctypes as C
import hashlib
import json
import statistics
import time
from pathlib import Path
from compare_sdk_work import Decoder
from verify_coefficient_temporal import inspect, run


def guarded_decode(library, path, backend):
    decoder = Decoder(library, path, 10)
    decoder.dll.fajk_set_backend.argtypes = [C.c_void_p, C.c_uint32]
    hashes, samples = {}, []

    def decode(index):
        pts, duration = C.c_int64(), C.c_int64()
        start = time.perf_counter()
        status = decoder.dll.fajk_decode_frame(decoder.handle, index,
            C.byref(decoder.buffer, 16), decoder.size, decoder.info.width * 4,
            C.byref(pts), C.byref(duration))
        elapsed = (time.perf_counter() - start) * 1000
        assert status == 0, decoder.dll.fajk_last_error(decoder.handle)
        return decoder.digest(), pts.value, duration.value, elapsed

    try:
        assert decoder.dll.fajk_set_backend(decoder.handle, backend) == 0
        count = decoder.info.frame_count
        for index in range(count):
            digest, pts, duration, elapsed = decode(index)
            hashes[index] = digest, pts, duration
            samples.append(elapsed)
        decoder.close()
        decoder = Decoder(library, path, 10)
        decoder.dll.fajk_set_backend.argtypes = [C.c_void_p, C.c_uint32]
        assert decoder.dll.fajk_set_backend(decoder.handle, backend) == 0
        seeks = []
        for index in [count - 1, 1, 0, count // 2, count - 1, 7 if count > 7 else 0]:
            digest, pts, duration, elapsed = decode(index)
            assert (digest, pts, duration) == hashes[index], (path, backend, index)
            seeks.append(dict(frame=index, ms=elapsed))
        return dict(backend=backend, all_frames=count, mean_ms=statistics.mean(samples),
                    p95_ms=sorted(samples)[int((count - 1) * .95)], fresh_random_seeks=seeks,
                    guards_and_timestamps=True)
    finally:
        decoder.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ['cli', 'library', 'video', 'output']:
        parser.add_argument('--' + flag, type=Path, required=True)
    parser.add_argument('--hold', type=Path)
    parser.add_argument('--frames', type=int, default=240)
    parser.add_argument('--repeats', type=int, default=2)
    args = parser.parse_args()
    assert args.frames >= 2 and args.repeats >= 1
    args.output.mkdir(parents=True, exist_ok=True)
    records = []

    def save(complete=False):
        (args.output / 'results.json').write_text(json.dumps(dict(complete=complete,
            frames=args.frames, repeats=args.repeats, records=records), indent=2))

    for backend, budget, tolerances in [(1, 80000, [6, 12]), (1, 30000, [6, 12]), (0, 80000, [6])]:
        group = []
        for tolerance in [None] + tolerances:
            mode = 'intra' if tolerance is None else f'max{tolerance}'
            path = args.output / f'{budget}-{"gpu" if backend else "cpu"}-{mode}.fajkv'
            group.append(dict(backend=backend, budget=budget, tolerance=tolerance,
                              path=str(path), encode_measurements=[], sha256=None))
        for repeat in range(args.repeats):
            for record in (group if repeat % 2 == 0 else reversed(group)):
                command = [args.cli, 'encode', args.video, record['path'], '--fast', '--no-aq',
                    '--protect-text', '0', '--gpu' if backend else '--cpu', '--workers', '10',
                    '--420', '--target-bytes', budget, '--max-frames', args.frames, '--overwrite']
                command += ['--intra-only'] if record['tolerance'] is None else [
                    '--temporal-max', '--similar-tolerance', record['tolerance']]
                record['encode_measurements'].append(run(command))
                digest = hashlib.sha256(Path(record['path']).read_bytes()).hexdigest()
                assert record['sha256'] in [None, digest], 'Nondeterministic repeated export'
                record['sha256'] = digest
                print('export', backend, budget, record['tolerance'], repeat,
                      record['encode_measurements'][-1], flush=True)
        for record in group:
            packets = inspect(Path(record['path']))
            assert len(packets) == args.frames and all(p['size'] <= budget for p in packets)
            assert max(p['depth'] for p in packets) <= 16
            record.update(bytes=Path(record['path']).stat().st_size,
                keyframes=sum(not p['reference'] for p in packets),
                max_depth=max(p['depth'] for p in packets),
                encode_median_ms=statistics.median(x['wall_ms'] for x in record['encode_measurements']))
        for record in group[1:]:
            record['saving_percent'] = 100 * (1 - record['bytes'] / group[0]['bytes'])
        records.extend(group)
        save()

    if args.hold:
        path = args.output / 'hold-max6.fajkv'
        timing = run([args.cli, 'encode', args.hold, path, '--fast', '--gpu', '--workers', 10,
            '--no-aq', '--protect-text', 0, '--420', '--target-bytes', 4000,
            '--max-frames', 420, '--overwrite', '--temporal-max', '--similar-tolerance', 6])
        packets = inspect(path)
        assert len(packets) == 420
        assert [n for n, p in enumerate(packets) if not p['reference']] == [0, 360]
        assert all(packets[n]['size'] == 8 for n in list(range(2, 360)) + list(range(361, 420)))
        records.append(dict(backend=1, budget=4000, tolerance=6, scenario='hold',
            path=str(path), bytes=path.stat().st_size, keyframes=2,
            encode_measurements=[timing], max_depth=max(p['depth'] for p in packets)))
        save()

    # Export timing is finished before the first DLL is loaded.
    for record in records:
        if record['tolerance'] is not None:
            record['sdk'] = [guarded_decode(args.library, Path(record['path']), backend)
                             for backend in [0, 1]]
            print('PASS SDK', record['path'], flush=True)
            save()
    save(True)
    print('PASS exports, repeat determinism, CRC/index/budgets, five-second PTS limit, '
          'bounded chains, long holds, every-frame SDK guards, timestamps and fresh seeks', flush=True)


if __name__ == '__main__':
    main()
