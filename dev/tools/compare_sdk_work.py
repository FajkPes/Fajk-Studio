"""Alternate native CPU decoders; record wall time, process CPU time and cycles.

CPU work is supplementary evidence under competing application load. It does
not replace end-to-end wall time or prove a frequency-independent speedup.
Only this explicit benchmark samples Windows process counters. Production
encode/decode and Debug never call them.
"""
import argparse
import ctypes as C
import hashlib
import json
import statistics
import time
from pathlib import Path


class Info(C.Structure):
    _fields_ = [('struct_size', C.c_uint32), ('width', C.c_uint32),
               ('height', C.c_uint32), ('fps_num', C.c_uint32),
               ('fps_den', C.c_uint32), ('source_step', C.c_uint32),
               ('frame_count', C.c_uint64), ('duration', C.c_int64)]


kernel = C.WinDLL('kernel32', use_last_error=True)
kernel.GetCurrentProcess.restype = C.c_void_p
kernel.QueryProcessCycleTime.argtypes = [C.c_void_p, C.POINTER(C.c_uint64)]
kernel.GetProcessTimes.argtypes = [C.c_void_p] + [C.POINTER(C.c_uint64)] * 4
process = kernel.GetCurrentProcess()


def counters():
    cycles, created, ended, system, user = (C.c_uint64() for _ in range(5))
    if not kernel.QueryProcessCycleTime(process, C.byref(cycles)):
        raise C.WinError(C.get_last_error())
    if not kernel.GetProcessTimes(process, C.byref(created), C.byref(ended),
                                  C.byref(system), C.byref(user)):
        raise C.WinError(C.get_last_error())
    return cycles.value, system.value + user.value


class Decoder:
    def __init__(self, library, path, workers):
        self.dll = C.CDLL(str(library.resolve()))
        self.dll.fajk_open.argtypes = [C.c_char_p, C.c_char_p, C.c_size_t]
        self.dll.fajk_open.restype = C.c_void_p
        self.dll.fajk_get_info.argtypes = [C.c_void_p, C.POINTER(Info)]
        self.dll.fajk_set_threads.argtypes = [C.c_void_p, C.c_uint32]
        self.dll.fajk_close.argtypes = [C.c_void_p]
        self.dll.fajk_last_error.argtypes = [C.c_void_p]
        self.dll.fajk_last_error.restype = C.c_char_p
        self.dll.fajk_decode_frame.argtypes = [C.c_void_p, C.c_uint64, C.c_void_p,
                                              C.c_size_t, C.c_size_t, C.c_void_p, C.c_void_p]
        message = C.create_string_buffer(1024)
        self.handle = self.dll.fajk_open(str(path.resolve()).encode('utf-8'), message, len(message))
        if not self.handle:
            raise RuntimeError(message.value)
        try:
            self.info = Info(struct_size=C.sizeof(Info))
            if self.dll.fajk_get_info(self.handle, C.byref(self.info)):
                raise RuntimeError(self.dll.fajk_last_error(self.handle))
            if self.dll.fajk_set_threads(self.handle, workers):
                raise RuntimeError(self.dll.fajk_last_error(self.handle))
            self.size = self.info.width * self.info.height * 4
            self.buffer = (C.c_uint8 * (self.size + 32))()
            C.memset(self.buffer, 197, len(self.buffer))
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.handle:
            self.dll.fajk_close(self.handle)
            self.handle = None

    def decode(self):
        if self.dll.fajk_decode_frame(self.handle, 0, C.byref(self.buffer, 16), self.size,
                                      self.info.width * 4, None, None):
            raise RuntimeError(self.dll.fajk_last_error(self.handle))

    def digest(self):
        raw = bytes(self.buffer)
        if raw[:16] != bytes([197]) * 16 or raw[-16:] != bytes([197]) * 16:
            raise RuntimeError('Decode buffer guard damaged')
        return hashlib.sha256(raw[16:-16]).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('before', 'after', 'packets', 'output'):
        parser.add_argument('--' + flag, type=Path, required=True)
    parser.add_argument('--samples', nargs='+', default=['22920', '22925', '23006'])
    parser.add_argument('--budgets', nargs='+', type=int, default=[30000, 80000, 150000])
    parser.add_argument('--repeats', type=int, default=32)
    parser.add_argument('--workers', type=int, default=0)
    parser.add_argument('--before-workers', type=int)
    parser.add_argument('--after-workers', type=int)
    args = parser.parse_args()
    if args.repeats < 8:
        parser.error('Use at least eight paired measurements.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    results = []
    for sample in args.samples:
        for budget in args.budgets:
            path = args.packets / f'{sample}-{budget}.fajk'
            decoders = {}
            try:
                for label in ('before', 'after'):
                    explicit = getattr(args, label + '_workers')
                    decoders[label] = Decoder(getattr(args, label), path, args.workers if explicit is None else explicit)
                records = {label: [] for label in decoders}
                for iteration in range(-4, args.repeats):
                    for label in (('before', 'after') if iteration % 2 == 0 else ('after', 'before')):
                        start_cycles, start_cpu = counters()
                        start_wall = time.perf_counter_ns()
                        decoders[label].decode()
                        end_wall = time.perf_counter_ns()
                        end_cycles, end_cpu = counters()
                        if iteration >= 0:
                            records[label].append({'wall_ms': (end_wall-start_wall)/1e6,
                                'process_cpu_ms': (end_cpu-start_cpu)/1e4, 'cycles': end_cycles-start_cycles})
                row = dict(sample=sample, budget=budget, workers=args.workers,
                           before_workers=args.before_workers, after_workers=args.after_workers,
                           identical_pixels=decoders['before'].digest() == decoders['after'].digest())
                for label, entries in records.items():
                    row[label] = {key: statistics.median(v[key] for v in entries)
                                  for key in ('wall_ms', 'cycles')}
                    row[label]['wall_p95_ms'] = sorted(v['wall_ms'] for v in entries)[int(.95*(len(entries)-1))]
                    # CPU times have coarse Windows accounting granularity.
                    row[label]['mean_process_cpu_ms'] = statistics.mean(v['process_cpu_ms'] for v in entries)
                    row[label]['measurements'] = entries
                results.append(row)
                args.output.write_text(json.dumps(results, indent=2), encoding='utf-8')
                print(json.dumps({k: v for k, v in row.items() if k not in ('before', 'after')}) +
                      f" wall {row['before']['wall_ms']:.3f}->{row['after']['wall_ms']:.3f};" +
                      f" cycles {row['before']['cycles']}->{row['after']['cycles']}", flush=True)
            finally:
                for decoder in decoders.values():
                    decoder.close()


if __name__ == '__main__':
    main()
