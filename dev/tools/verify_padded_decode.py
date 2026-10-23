"""Paired SDK timings for contiguous/padded rows, with exact pixels and guards."""
import argparse, ctypes as C, hashlib, json, statistics, time
from pathlib import Path
from compare_sdk_work import Decoder

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['before','after','output']:p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--inputs',type=Path,nargs='+',required=True)
    p.add_argument('--repeats',type=int,default=24)
    a=p.parse_args();assert a.repeats>=16
    a.output.mkdir(parents=True,exist_ok=True);records=[]
    for source in a.inputs:
        for workers in [2,10]:
            for backend in [0,1]:
                for padding in [0,28]:
                    decoders=[Decoder(dll,source,workers) for dll in [a.before,a.after]]
                    outputs=[];samples=[[],[]];metadata=[]
                    try:
                        for decoder in decoders:
                            decoder.dll.fajk_set_backend.argtypes=[C.c_void_p,C.c_uint32]
                            assert not decoder.dll.fajk_set_backend(decoder.handle,backend)
                            w,h=decoder.info.width,decoder.info.height;stride=w*4+padding;capacity=stride*(h-1)+w*4
                            buffer=(C.c_uint8*(capacity+32))();C.memset(buffer,197,len(buffer));outputs.append(buffer)
                            metadata.append((C.c_int64(),C.c_int64()))
                        for n in range(-4,a.repeats):
                            index=(n+4)%decoders[0].info.frame_count
                            for i in ([0,1] if n%2==0 else [1,0]):
                                decoder=decoders[i];pts,duration=metadata[i]
                                start=time.perf_counter_ns()
                                error=decoder.dll.fajk_decode_frame(decoder.handle,index,C.byref(outputs[i],16),capacity,stride,C.byref(pts),C.byref(duration))
                                elapsed=(time.perf_counter_ns()-start)/1e6
                                if error:raise RuntimeError(decoder.dll.fajk_last_error(decoder.handle))
                                if n>=0:samples[i].append(elapsed)
                        hashes=[]
                        for output in outputs:
                            raw=bytes(output);assert raw[:16]==raw[-16:]==bytes([197])*16
                            pixels=bytearray()
                            for y in range(h):
                                at=16+y*stride;pixels+=raw[at:at+w*4]
                                if y+1<h:assert raw[at+w*4:at+stride]==bytes([197])*padding
                            hashes.append(hashlib.sha256(pixels).hexdigest())
                        assert hashes[0]==hashes[1]
                        assert [(pts.value,duration.value) for pts,duration in metadata][0]==[(pts.value,duration.value) for pts,duration in metadata][1]
                        row=dict(input=str(source),workers=workers,backend=backend,padding=padding,identical_pixels=True,
                                 before_ms=statistics.mean(samples[0]),after_ms=statistics.mean(samples[1]),samples=samples)
                        records.append(row);(a.output/'results.json').write_text(json.dumps(dict(complete=False,cases=records),indent=2),encoding='utf-8')
                        print(f'{source.name} w{workers} b{backend} padding{padding}: {row["before_ms"]:.3f} -> {row["after_ms"]:.3f} ms',flush=True)
                    finally:
                        for decoder in decoders:decoder.close()
    (a.output/'results.json').write_text(json.dumps(dict(complete=True,cases=records),indent=2),encoding='utf-8')
    print('PASS paired padded SDK decode, exact pixels, metadata, padding and guards')

if __name__=='__main__':main()
