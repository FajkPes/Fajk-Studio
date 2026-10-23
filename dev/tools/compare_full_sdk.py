"""Alternate two decoder DLLs over every packet of complete CPU/GPU videos.

Each long timed pass includes packet reads/CRC, transfers, waits and RGBA
reconstruction through the C ABI. All output buffers and handles are reused.
Both builds are in the same process; only one is decoding at a time. Compare
with full regression checks separately; this timing pass hashes the last frame.
"""
import argparse
import ctypes as C
import hashlib
import json
import statistics
import time
from pathlib import Path
from compare_sdk_work import Decoder, counters


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ['before', 'after', 'packets', 'output']:
        p.add_argument('--'+key, type=Path, required=True)
    p.add_argument('--rounds', type=int, default=2)
    p.add_argument('--workers', type=int, default=10)
    a = p.parse_args()
    if a.rounds < 2:
        p.error('Use at least two rounds.')
    a.output.mkdir(parents=True, exist_ok=True)
    sha = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
    report = dict(method=__doc__, before_sha256=sha(a.before), after_sha256=sha(a.after),
                  rounds=a.rounds, workers=a.workers, records=[], complete=False)
    for backend in ['cpu','gpu']:
        decoders = {}
        try:
            packet = a.packets/f'full-{backend}.fajkv'
            for label in ['before','after']:
                d = Decoder(getattr(a,label), packet, a.workers)
                decoders[label] = d
                d.dll.fajk_set_backend.argtypes = [C.c_void_p,C.c_uint32]
                if d.dll.fajk_set_backend(d.handle, int(backend=='gpu')):
                    raise RuntimeError('Cannot select SDK backend')
                for frame in [0,d.info.frame_count//2,d.info.frame_count-1]:
                    if d.dll.fajk_decode_frame(d.handle,frame,C.byref(d.buffer,16),d.size,d.info.width*4,None,None):
                        raise RuntimeError(d.dll.fajk_last_error(d.handle))
            for turn in range(a.rounds):
                for label in (['before','after'] if turn%2==0 else ['after','before']):
                    d=decoders[label]
                    begin_cycles,begin_cpu=counters();start=time.perf_counter_ns()
                    for frame in range(d.info.frame_count):
                        if d.dll.fajk_decode_frame(d.handle,frame,C.byref(d.buffer,16),d.size,d.info.width*4,None,None):
                            raise RuntimeError(d.dll.fajk_last_error(d.handle))
                    elapsed=(time.perf_counter_ns()-start)/1e6;end_cycles,end_cpu=counters()
                    record=dict(backend=backend,label=label,round=turn,frames=d.info.frame_count,
                                wall_ms=elapsed,cpu_ms=(end_cpu-begin_cpu)/10000.,
                                cycles=end_cycles-begin_cycles,last_rgba_sha256=d.digest())
                    report['records'].append(record)
                    (a.output/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
                    print(backend,turn,label,round(elapsed,1),'ms',round(record['cpu_ms'],1),'CPU ms',flush=True)
            signatures={r['last_rgba_sha256'] for r in report['records'] if r['backend']==backend}
            assert len(signatures)==1,'Last decoded frame changed'
        finally:
            for d in decoders.values():d.close()
    report['summary']={}
    for backend in ['cpu','gpu']:
        values={}
        for label in ['before','after']:
            rows=[r for r in report['records'] if r['backend']==backend and r['label']==label]
            values[label]={k:statistics.median(r[k] for r in rows) for k in ['wall_ms','cpu_ms','cycles']}
        values['ratios']={k:values['after'][k]/values['before'][k] for k in values['before']}
        report['summary'][backend]=values
    report['complete']=True
    (a.output/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')


if __name__=='__main__':
    main()
