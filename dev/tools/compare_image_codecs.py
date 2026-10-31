"""Matched whole-file limits, actual pixels and repeated in-memory codec timings.

FAJK includes its internal size search. Competitor final-parameter timings
exclude external parameter search, reported separately. Presets are explicit,
not matched effort. AVIF/JXL include API output allocation/copies; FAJK writes
to a prepared RGBA buffer. CLI startup and PNG IO are excluded from all timings.
"""
import argparse,configparser,ctypes as C,csv,hashlib,io,json,statistics,subprocess,time
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,AvifImagePlugin,_avif,__version__ as pillow_version
from compare_decode import assess_quality

class Buffer(C.Structure):
    _fields_=[('data',C.c_void_p),('size',C.c_size_t)]

class Jxl:
    def __init__(self,path,threads,effort):
        self.lib=C.CDLL(str(path.resolve()));self.threads=threads;self.effort=effort
        self.lib.compare_jxl_error.restype=C.c_char_p
        self.lib.compare_jxl_free.argtypes=[C.c_void_p]
        self.lib.compare_jxl_encode.argtypes=[C.c_void_p,C.c_uint,C.c_uint,C.c_float,C.c_int,C.c_uint,C.c_int,C.POINTER(Buffer)]
        self.lib.compare_jxl_decode.argtypes=[C.c_void_p,C.c_size_t,C.c_uint,C.POINTER(Buffer),C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    def result(self,status,out):
        if status: raise RuntimeError(self.lib.compare_jxl_error().decode())
        try:return C.string_at(out.data,out.size)
        finally:self.lib.compare_jxl_free(out.data)
    def encode(self,image,distance,resampling=-1):
        raw=image.tobytes();out=Buffer()
        return self.result(self.lib.compare_jxl_encode(raw,*image.size,distance,self.effort,self.threads,resampling,C.byref(out)),out)
    def decode(self,data):
        out=Buffer();w=C.c_uint();h=C.c_uint()
        raw=self.result(self.lib.compare_jxl_decode(data,len(data),self.threads,C.byref(out),C.byref(w),C.byref(h)),out)
        return Image.frombytes('RGBA',(w.value,h.value),raw)

def timing(values):
    return dict(median_ms=statistics.median(values),p95_ms=float(np.percentile(values,95)),samples_ms=values)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cli',type=Path,required=True);p.add_argument('--jxl',type=Path,required=True)
    p.add_argument('--preset',type=Path,required=True);p.add_argument('--threads',type=int,default=10)
    p.add_argument('--avif-speed',type=int,default=6);p.add_argument('--jxl-effort',type=int,default=7)
    p.add_argument('--repeats',type=int,default=8);p.add_argument('--budgets',type=int,nargs='+',default=[30000,80000,150000])
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    reference=Image.open(a.input).convert('RGB');reference.info.clear();reference.save(a.output/'reference.png')
    source_hash=hashlib.sha256(reference.tobytes()).hexdigest()
    AvifImagePlugin.DEFAULT_MAX_THREADS=a.threads;AvifImagePlugin.DECODE_CODEC_CHOICE='dav1d'
    jxl=Jxl(a.jxl,a.threads,a.jxl_effort)
    cfg=configparser.ConfigParser();cfg.read(a.preset,encoding='utf-8-sig');settings=dict(cfg['Studio'])
    avif_settings=dict(speed=a.avif_speed,subsampling='4:2:0',max_threads=a.threads,codec='aom',range='full')
    def avif_encode(q,scale=1.):
        image=reference if scale==1 else reference.resize((round(reference.width*scale),round(reference.height*scale)),Image.Resampling.LANCZOS)
        out=io.BytesIO();image.save(out,format='AVIF',quality=q,**avif_settings);return out.getvalue()
    def avif_decode(data):
        with Image.open(io.BytesIO(data)) as im:im.load();result=im.convert('RGBA')
        return result if result.size==reference.size else result.resize(reference.size,Image.Resampling.LANCZOS)
    report=dict(source=str(a.input),size=reference.size,rgb_sha256=source_hash,threads=a.threads,
        pillow=pillow_version,libavif=_avif.libavif_version,avif_codecs=_avif.codec_versions(),
        libjxl_version=jxl.lib.compare_jxl_version(),preset=settings,results=[],
        method=__doc__,time=time.strftime('%Y-%m-%d %H:%M:%S'))
    def save_report():
        (a.output/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    # Search once, record every actual candidate. Files never exceed their cap.
    jobs={}
    for budget in a.budgets:
        start=time.perf_counter();trials=[];best=None;lo,hi=0,100;scale=1.
        endpoint=avif_encode(0)
        if len(endpoint)>budget:
            (a.output/f'{budget}-avif-native-minimum.avif').write_bytes(endpoint)
            with Image.open(io.BytesIO(endpoint)) as native:native.load();native.save(a.output/f'{budget}-avif-native-minimum.png')
            for candidate in (.875,.75,.625,.5):
                endpoint=avif_encode(0,candidate)
                if len(endpoint)<=budget:scale=candidate;break
            else:raise RuntimeError('AVIF minimum cannot fit even at half resolution')
        while lo<=hi:
            q=(lo+hi)//2;data=avif_encode(q,scale);trials.append(dict(quality=q,bytes=len(data)))
            print('AVIF search',budget,q,len(data),flush=True)
            if len(data)<=budget:best=(q,data);lo=q+1
            else:hi=q-1
        if best is None:raise RuntimeError(f'AVIF cannot fit {budget} at quality 0')
        jobs[(budget,'AVIF')]=dict(parameter=best[0],scale=scale,data=best[1],search_ms=(time.perf_counter()-start)*1000,trials=trials)
        start=time.perf_counter();trials=[];best=None
        # The library's normal range ends at distance 25. Use its documented
        # internal resampling only if the minimum still cannot reach the cap.
        resampling=-1
        for scale in (-1,2,4,8):
            data=jxl.encode(reference,25.,scale);trials.append(dict(distance=25.,resampling=scale,bytes=len(data)))
            print('JXL endpoint',budget,scale,len(data),flush=True)
            if len(data)<=budget:resampling=scale;best=(25.,data);break
        if best is None:raise RuntimeError(f'JXL cannot fit {budget}')
        lo,hi=.01,25.
        for _ in range(12):
            d=(lo+hi)/2;data=jxl.encode(reference,d,resampling);trials.append(dict(distance=d,resampling=resampling,bytes=len(data)))
            print('JXL search',budget,round(d,4),resampling,len(data),flush=True)
            if len(data)<=budget:best=(d,data);hi=d
            else:lo=d
        jobs[(budget,'JPEG XL')]=dict(parameter=best[0],resampling=resampling,data=best[1],search_ms=(time.perf_counter()-start)*1000,trials=trials)
        for codec,suffix in [('AVIF','avif'),('JPEG XL','jxl')]:
            item=jobs[budget,codec];(a.output/f'{budget}-fitted.{suffix}').write_bytes(item['data'])
            (a.output/f'{budget}-fitted-{suffix}.json').write_text(json.dumps({k:v for k,v in item.items() if k!='data'},indent=2),encoding='utf-8')
    # Warm all APIs first, then alternate order by budget and repeat.
    for budget in a.budgets:
        images={'Original':reference};rows={}
        prefix=a.output/f'{budget}-fajk'
        subprocess.run([str(a.cli.resolve()),'measure',str((a.output/'reference.png').resolve()),str(prefix.resolve()),'100',str(a.repeats),
            '420',str(a.threads),'100','wavelet',str(budget-168),'0','r470-buffer',settings.get('flatallocation','50'),'1',
            settings.get('flatsavings','0'),settings.get('flatthreshold','0'),str(a.preset.resolve())],check=True,stdout=subprocess.DEVNULL)
        samples=list(csv.DictReader(prefix.with_suffix('.csv').open()))
        images['FAJK']=Image.open(prefix.with_suffix('.png')).convert('RGB')
        rows['FAJK']=dict(codec='FAJK',budget=budget,bytes=prefix.with_suffix('.fajkv').stat().st_size,
            encode=timing([float(x['encode_ms']) for x in samples]),decode=timing([float(x['decode_ms']) for x in samples]),
            settings=settings,search_ms=None,file=prefix.with_suffix('.fajkv').name,png=prefix.with_suffix('.png').name)
        for codec in ('AVIF','JPEG XL'):
            job=jobs[budget,codec];data=job['data']
            encode=(lambda:avif_encode(job['parameter'],job['scale'])) if codec=='AVIF' else (lambda:jxl.encode(reference,job['parameter'],job['resampling']))
            decode=avif_decode if codec=='AVIF' else jxl.decode
            # No PNG writes, quality metric or file reads inside measured calls.
            encode();decode(data);rows[codec]=dict(codec=codec,budget=budget,bytes=len(data),encode_samples=[],decode_samples=[],
                parameter=job['parameter'],search_ms=job['search_ms'],search_trials=job['trials'],
                settings={**avif_settings,'input_scale':job['scale'],'encoded_size':[round(reference.width*job['scale']),round(reference.height*job['scale'])],'resampling_timed':job['scale']!=1} if codec=='AVIF' else dict(effort=a.jxl_effort,resampling=job['resampling'],container=True))
        for repeat in range(a.repeats):
            for codec in (('AVIF','JPEG XL') if repeat%2==0 else ('JPEG XL','AVIF')):
                job=jobs[budget,codec];data=job['data'];t=time.perf_counter()
                encoded=avif_encode(job['parameter'],job['scale']) if codec=='AVIF' else jxl.encode(reference,job['parameter'],job['resampling'])
                rows[codec]['encode_samples'].append((time.perf_counter()-t)*1000)
                assert len(encoded)==len(data) and encoded==data,'Nondeterministic encoding'
                t=time.perf_counter();decoded=avif_decode(data) if codec=='AVIF' else jxl.decode(data)
                rows[codec]['decode_samples'].append((time.perf_counter()-t)*1000)
                assert decoded.size==reference.size
                images[codec]=decoded.convert('RGB')
        for codec,suffix in [('AVIF','avif'),('JPEG XL','jxl')]:
            row=rows[codec];row['encode']=timing(row.pop('encode_samples'));row['decode']=timing(row.pop('decode_samples'))
            base=a.output/f'{budget}-{suffix}';base.with_suffix('.'+suffix).write_bytes(jobs[budget,codec]['data'])
            images[codec].save(base.with_suffix('.png'));row['file']=base.with_suffix('.'+suffix).name;row['png']=base.with_suffix('.png').name
        for codec,row in rows.items():
            assert row['bytes']<=budget
            metric=assess_quality(a.output/'reference.png',a.output/row['png'],a.output/row['png'],a.cli.parent/'fajk_metric.exe')
            row['ssimulacra2']=metric['ss2_after'];row['rgb_mae']=metric['rgb_mae_after'];row['edge_mae']=metric['edge_band_mae_after']
            report['results'].append(row);print(json.dumps({k:v for k,v in row.items() if k not in ('search_trials','settings')}),flush=True)
        save_report()
        # Every image gets identical display geometry. Full PNGs remain native.
        for label,box in [('dog',(1620,420,2460,1800)),('fur',(1760,470,2272,982)),('text',(150,980,1320,1260))]:
            w,h=box[2]-box[0],box[3]-box[1];canvas=Image.new('RGB',(w*4,h+68),'#111111');draw=ImageDraw.Draw(canvas)
            for i,codec in enumerate(('Original','FAJK','AVIF','JPEG XL')):
                canvas.paste(images[codec].crop(box),(i*w,68));draw.text((i*w+8,8),codec+(f" / {rows[codec]['bytes']} B" if codec!='Original' else ''),fill='white')
                if codec!='Original':draw.text((i*w+8,28),f"ENC {rows[codec]['encode']['median_ms']:.2f} ms / DEC {rows[codec]['decode']['median_ms']:.2f} ms",fill='white')
            canvas.save(a.output/f'{budget}-{label}-comparison.png')
        overview=Image.new('RGB',(1920,1160),'#111111');draw=ImageDraw.Draw(overview)
        for i,codec in enumerate(('Original','FAJK','AVIF','JPEG XL')):
            x=(i%2)*960;y=(i//2)*580
            draw.text((x+8,y+8),codec+(f" / {rows[codec]['bytes']} B" if codec!='Original' else ''),fill='white')
            overview.paste(images[codec].resize((960,540),Image.Resampling.LANCZOS),(x,y+40))
        overview.save(a.output/f'{budget}-overview.png')
    print('DONE',flush=True)
if __name__=='__main__':main()
