"""Paired sequential/fresh-seek SDK timings, exact pixels and buffer guards.

Before/after files may differ in losslessly compacted motion metadata. This
check demands identical pixels per backend by default. A deliberately lossy
candidate needs --allow-pixel-differences and a separate source-quality probe.
Each timing excludes Python hashing and process setup. Run without concurrent
exports, builds or quality probes; compare means over the whole sequence.
"""
import argparse
import ctypes as C
import json
import hashlib
import os
import platform
import statistics
import time
from pathlib import Path
from compare_sdk_work import Decoder, counters, kernel


def machine_cpu():
    kernel.GetSystemTimes.argtypes = [C.POINTER(C.c_uint64)] * 3
    idle, system, user = (C.c_uint64() for _ in range(3))
    if not kernel.GetSystemTimes(C.byref(idle), C.byref(system), C.byref(user)):
        raise C.WinError(C.get_last_error())
    return idle.value, system.value + user.value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['before', 'after', 'before-video', 'after-video', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--workers', type=int, default=10)
    parser.add_argument('--sharpening', type=int, choices=range(101), default=0,
                        help='Final display sharpening; 50 matches the usual Studio display')
    parser.add_argument('--warmup', type=int, default=0,
                        help='Decode this many initial frames per decoder before each measured pass')
    parser.add_argument('--backends', type=int, choices=(0, 1), nargs='+', default=[0, 1])
    parser.add_argument('--cpu-work', action='store_true',
                        help='Also sample whole-process CPU work around each native decode, excluding Python hashing')
    parser.add_argument('--allow-pixel-differences', action='store_true',
                        help='Time quality-changing exports; still require timestamps, guards and independent determinism')
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error('Warmup must be nonnegative.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    records = []
    inputs = {name: dict(path=str(path.resolve()), bytes=path.stat().st_size,
                        sha256=hashlib.sha256(path.read_bytes()).hexdigest())
              for name, path in [('before_library', args.before), ('after_library', args.after),
                                 ('before_video', args.before_video), ('after_video', args.after_video)]}

    def save(complete=False):
        args.output.write_text(json.dumps(dict(complete=complete, workers=args.workers,
            repeats=args.repeats, sharpening=args.sharpening, warmup_frames=args.warmup, cpu_work_sampled=args.cpu_work,
            inputs=inputs, environment=dict(os=platform.platform(), logical_cpus=os.cpu_count()), records=records), indent=2))

    def open_pair(backend):
        pair = []
        try:
            for dll, video in [(args.before, args.before_video), (args.after, args.after_video)]:
                d = Decoder(dll, video, args.workers)
                pair.append(d)
                d.dll.fajk_set_backend.argtypes = [C.c_void_p, C.c_uint32]
                assert d.dll.fajk_set_backend(d.handle, backend) == 0
                d.dll.fajk_set_sharpening.argtypes = [C.c_void_p, C.c_uint32]
                assert d.dll.fajk_set_sharpening(d.handle, args.sharpening) == 0
            assert pair[0].info.frame_count == pair[1].info.frame_count
            return pair
        except BaseException:
            for d in pair:
                d.close()
            raise

    def decode(d, index):
        pts, duration = C.c_int64(), C.c_int64()
        before = counters() if args.cpu_work else None
        start = time.perf_counter_ns()
        status = d.dll.fajk_decode_frame(d.handle, index, C.byref(d.buffer, 16),
            d.size, d.info.width * 4, C.byref(pts), C.byref(duration))
        elapsed = (time.perf_counter_ns() - start) / 1e6
        cpu_ms = (counters()[1] - before[1]) / 10000 if before is not None else None
        assert status == 0, d.dll.fajk_last_error(d.handle)
        return elapsed, (d.digest(), pts.value, duration.value), cpu_ms

    for backend in args.backends:
        samples = [[], []]
        cpu_samples = [[], []]
        hashes = [{}, {}]
        different = set()
        row = dict(backend=backend, frames=0, exact_pixels=True, fresh_seeks=[], machine_load=[])
        records.append(row)
        for repeat in range(args.repeats):
            pair = open_pair(backend)
            try:
                count = pair[0].info.frame_count
                for decoder in pair:
                    for index in range(min(args.warmup, count)):
                        decode(decoder, index)
                # Keep sequential reference reuse even when both streams use
                # the same DLL and its thread-local reference cache.
                for which in ([0, 1] if repeat % 2 == 0 else [1, 0]):
                    machine_before, cpu_before, pass_start = machine_cpu(), counters()[1], time.perf_counter()
                    for index in range(count):
                        elapsed, result, cpu_ms = decode(pair[which], index)
                        samples[which].append(elapsed)
                        if cpu_ms is not None:
                            cpu_samples[which].append(cpu_ms)
                        assert hashes[which].get(index, result) == result, 'Nondeterministic repeated decode'
                        hashes[which][index] = result
                    machine_after, cpu_after = machine_cpu(), counters()[1]
                    total = machine_after[1] - machine_before[1]
                    busy = total - (machine_after[0] - machine_before[0])
                    process_work = cpu_after - cpu_before
                    row['machine_load'].append(dict(repeat=repeat, decoder=['before', 'after'][which],
                        wall_ms=(time.perf_counter() - pass_start) * 1000,
                        system_cpu_percent=100 * busy / total if total else None,
                        process_cpu_percent=100 * process_work / total if total else None,
                        estimated_other_cpu_percent=100 * max(0, busy - process_work) / total if total else None))
                for index in range(count):
                    before, after = hashes[0][index], hashes[1][index]
                    assert before[1:] == after[1:], ('timestamp regression', backend, index)
                    if before[0] != after[0]:
                        different.add(index)
                        assert args.allow_pixel_differences, ('pixel regression', backend, index)
                row['frames'] += count
            finally:
                for d in pair:
                    d.close()
            print('PASS sequential', backend, repeat, count, flush=True)
        # Each seek starts with new handles, including a jump into a P chain.
        for index in [count - 1, 1, 0, count // 2, 7]:
            pair = open_pair(backend)
            try:
                times = []
                for which, d in enumerate(pair):
                    elapsed, result, _ = decode(d, min(index, count - 1))
                    assert result == hashes[which][min(index, count - 1)], 'Fresh seek regression'
                    times.append(elapsed)
                row['fresh_seeks'].append(dict(index=index, before_ms=times[0], after_ms=times[1]))
            finally:
                for d in pair:
                    d.close()
        row['exact_pixels'] = not different
        row['different_frames'] = len(different)
        row['quality_assessed'] = False
        for which, name in enumerate(['before', 'after']):
            row[name] = dict(mean_ms=statistics.mean(samples[which]),
                p95_ms=sorted(samples[which])[int(.95 * (len(samples[which]) - 1))])
            if args.cpu_work:
                row[name]['mean_process_cpu_ms'] = statistics.mean(cpu_samples[which])
        row['speedup'] = row['before']['mean_ms'] / row['after']['mean_ms']
        save()
        print('PASS backend', backend, row['before'], row['after'], flush=True)
    save(True)


if __name__ == '__main__':
    main()
