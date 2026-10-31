"""Compare real exports, every decoded frame and guarded native SDK output.

Timings include process startup, video input and output commits. CPU process
time is recorded separately. Pixel equality is checked per backend, so GPU
rounding is never mistaken for a temporal quality change.
"""
import argparse
import ctypes as C
import hashlib
import json
import statistics
import struct
import subprocess
import time
import zlib
from pathlib import Path
from compare_sdk_work import Decoder, kernel


def inspect(path):
    data = path.read_bytes()
    version, = struct.unpack_from('<I', data, 8)
    count, offset = struct.unpack_from('<QQ', data, 40)
    entries = data[offset:]
    assert len(entries) == count * 40
    assert zlib.crc32(entries) == int.from_bytes(data[64:68], 'little')
    assert zlib.crc32(data[:68]) == int.from_bytes(data[68:72], 'little')
    records = []
    for n in range(count):
        start, size, pts, duration, checksum, ref = struct.unpack_from('<QQqqII', entries, n * 40)
        payload = data[start:start + size]
        assert len(payload) == size and zlib.crc32(payload) == checksum
        assert bool(ref) == (payload[4] in [136,137])
        assert not ref or ref <= n
        depth = records[ref - 1]['depth'] + 1 if ref else 0
        root = records[ref - 1]['root'] if ref else n
        assert depth <= 32 and (version == 3 or not ref or records[ref - 1]['reference'] == 0)
        if ref and payload[5] != 2:
            assert pts - records[root]['pts'] < 50_000_000, 'Changed image exceeded five-second refresh bound'
        records.append(dict(reference=ref, pts=pts, duration=duration, size=size, depth=depth, root=root, mode=payload[5]))
    return records


def run(command):
    start = time.perf_counter()
    process = subprocess.Popen(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output, _ = process.communicate()
    elapsed = (time.perf_counter() - start) * 1000
    assert process.returncode == 0, output.decode(errors='replace')
    created, ended, system, user = (C.c_uint64() for _ in range(4))
    assert kernel.GetProcessTimes(int(process._handle), C.byref(created), C.byref(ended), C.byref(system), C.byref(user))
    return dict(wall_ms=elapsed, process_cpu_ms=(system.value + user.value) / 10000)


def decode_pair(library, intra, temporal, backend, count):
    decoders = [Decoder(library, path, 10) for path in [intra, temporal]]
    records = []
    try:
        for decoder in decoders:
            decoder.dll.fajk_set_backend.argtypes = [C.c_void_p, C.c_uint32]
            assert decoder.dll.fajk_set_backend(decoder.handle, backend) == 0
        order = list(range(count)) + [count - 1, 1, 0, count // 2, 7, count - 1, 1]
        for n, index in enumerate(order):
            hashes, timings, timestamps = {}, {}, {}
            for label in ([0, 1] if n % 2 == 0 else [1, 0]):
                decoder = decoders[label]
                pts, duration = C.c_int64(), C.c_int64()
                start = time.perf_counter()
                result = decoder.dll.fajk_decode_frame(decoder.handle, index, C.byref(decoder.buffer, 16),
                    decoder.size, decoder.info.width * 4, C.byref(pts), C.byref(duration))
                timings[label] = (time.perf_counter() - start) * 1000
                assert result == 0, decoder.dll.fajk_last_error(decoder.handle)
                hashes[label] = decoder.digest()
                timestamps[label] = (pts.value, duration.value)
            assert hashes[0] == hashes[1], f'Temporal changed pixels, backend {backend}, frame {index}'
            assert timestamps[0] == timestamps[1], f'Temporal changed timestamps, frame {index}'
            records.append(dict(frame=index, pixels_exact=True, timestamps_exact=True,
                                intra_ms=timings[0], temporal_ms=timings[1]))
    finally:
        for decoder in decoders:
            decoder.close()
    return records


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for flag in ['cli', 'library', 'video', 'screen', 'output']:
        p.add_argument('--' + flag, type=Path, required=True)
    p.add_argument('--hold', type=Path)
    p.add_argument('--frames', type=int, default=240)
    p.add_argument('--repeats', type=int, default=3)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    scenarios = [('gaming', a.video, a.frames, 80000), ('screen', a.screen, 32, 12000)]
    if a.hold:
        scenarios.append(('hold', a.hold, 420, 4000))
    results = []
    for name, source, count, budget in scenarios:
        for backend in [0, 1]:
            paths = [a.output / f'{name}-{"gpu" if backend else "cpu"}-{mode}.fajkv'
                     for mode in ['intra', 'coefficients']]
            timings, digests = [[], []], [None, None]
            for repeat in range(a.repeats):
                for mode in ([0, 1] if repeat % 2 == 0 else [1, 0]):
                    command = [a.cli, 'encode', source, paths[mode], '--fast', '--no-aq', '--protect-text', '0',
                               '--gpu' if backend else '--cpu', '--workers', '10', '--420',
                               '--target-bytes', str(budget), '--max-frames', str(count), '--overwrite',
                               '--temporal-coefficients' if mode else '--intra-only']
                    timings[mode].append(run(command))
                    digest = hashlib.sha256(paths[mode].read_bytes()).hexdigest()
                    assert digests[mode] in [None, digest], 'Repeated export is nondeterministic'
                    digests[mode] = digest
                    print(name, backend, repeat, mode, timings[mode][-1], flush=True)
            original, predicted = map(inspect, paths)
            assert len(original) == len(predicted) == count
            assert all(r['size'] <= budget for r in original + predicted)
            assert any(r['reference'] for r in predicted), 'No temporal frames were emitted'
            assert all((x['pts'], x['duration']) == (y['pts'], y['duration']) for x, y in zip(original, predicted))
            if name == 'hold':
                assert [n for n,r in enumerate(predicted) if not r['reference']] == [0,360], 'Long hold generated redundant full keyframes'
                assert all(predicted[n]['size'] == 8 for n in list(range(2,360)) + list(range(361,420)))
            decoded = decode_pair(a.library, *paths, backend, count)
            record = dict(scenario=name, backend=backend, frames=count, budget=budget,
                intra_bytes=paths[0].stat().st_size, temporal_bytes=paths[1].stat().st_size,
                keys=sum(not r['reference'] for r in predicted), max_depth=max(r['depth'] for r in predicted), encode_measurements=timings,
                all_pixels_exact=True, sha256=digests, decode_measurements=decoded)
            record['saving_percent'] = 100 * (1 - record['temporal_bytes'] / record['intra_bytes'])
            record['encode_median_ms'] = [statistics.median(r['wall_ms'] for r in group) for group in timings]
            record['decode_mean_ms'] = [statistics.mean(r[k] for r in decoded[:count]) for k in ['intra_ms', 'temporal_ms']]
            results.append(record)
            (a.output / 'results.json').write_text(json.dumps(dict(complete=False, records=results), indent=2))
            print('PASS', name, backend, 'saved', record['saving_percent'], 'every pixel exact', flush=True)
    (a.output / 'results.json').write_text(json.dumps(dict(complete=True, records=results), indent=2))
    print('PASS real exports, CRC/index/budgets, determinism, all frames, random seek, SDK guards and timestamps', flush=True)


if __name__ == '__main__':
    main()
