"""Check R490 SDK against matching CLI worker budgets, strides and concurrent handles."""
import argparse,ctypes as C,hashlib,json,subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from PIL import Image
from compare_sdk_work import Decoder

def check(library,packet,png,backend,workers,padding):
    decoder=Decoder(library,packet,workers)
    try:
        lib=decoder.dll;lib.fajk_set_backend.argtypes=[C.c_void_p,C.c_uint32]
        if lib.fajk_set_backend(decoder.handle,backend):raise RuntimeError('Cannot select SDK backend')
        if lib.fajk_set_backend(decoder.handle,2)!=-1:raise RuntimeError('SDK accepted invalid backend')
        # Invalid selection must preserve the previous valid backend.
        w,h=decoder.info.width,decoder.info.height;stride=w*4+padding;capacity=stride*(h-1)+w*4
        output=(C.c_uint8*(capacity+32))();C.memset(output,197,len(output));pts=C.c_int64();duration=C.c_int64()
        status=lib.fajk_decode_frame(decoder.handle,0,C.byref(output,16),capacity,stride,C.byref(pts),C.byref(duration))
        if status:raise RuntimeError(lib.fajk_last_error(decoder.handle))
        raw=bytes(output);guard=bytes([197])
        if raw[:16]!=guard*16 or raw[-16:]!=guard*16:raise RuntimeError('SDK damaged output guards')
        pixels=bytearray()
        for y in range(h):
            start=16+y*stride;pixels.extend(raw[start:start+w*4])
            if y<h-1 and raw[start+w*4:start+stride]!=guard*padding:raise RuntimeError('SDK damaged row padding')
        with Image.open(png) as image:expected=image.convert('RGBA').tobytes()
        if pixels!=expected:raise RuntimeError('SDK/CLI pixel mismatch on same backend')
        if pts.value<0 or duration.value<=0:raise RuntimeError('SDK timestamps invalid')
        return dict(file=packet.name,backend=backend,workers=workers,padding=padding,sha256=hashlib.sha256(pixels).hexdigest())
    finally:decoder.close()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library',type=Path,required=True);p.add_argument('--cli',type=Path,required=True)
    p.add_argument('--packets',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);jobs=[]
    packets=sorted(a.packets.glob('*-1-gpu.fajkv'))
    if not packets:raise RuntimeError('No GPU reference packets matched *-1-gpu.fajkv.')
    for packet in packets:
        for backend in [0,1]:
            for workers in [1,2,10]:
                # Worker budget selects GPU postfilter/upscale or CPU AVX2;
                # compare the exact same execution choice in both frontends.
                png=a.output/f'{packet.stem}-backend{backend}-workers{workers}.png'
                subprocess.run([str(a.cli),'decode',str(packet),'0',str(png),'--workers',str(workers),'--gpu' if backend else '--cpu'],check=True,capture_output=True)
                for padding in [0,28]:jobs.append((a.library,packet,png,backend,workers,padding))
    # Two independent decoder handles use one GPU engine safely while the CPU
    # coefficient jobs overlap. No singleton backend setting leaks between them.
    with ThreadPoolExecutor(max_workers=2) as pool:records=list(pool.map(lambda j:check(*j),jobs))
    (a.output/'sdk-results.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    print(f'PASS: {len(records)} SDK decodes, exact CLI pixels, stride guards, concurrent CPU/GPU handles.')

if __name__=='__main__':main()
