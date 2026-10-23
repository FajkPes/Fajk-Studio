"""Validate and compare Fast/quality video, preserving the strict native pipeline checks."""
import argparse,csv,hashlib,json,statistics,subprocess
from pathlib import Path
from verify_gpu_video import run

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for k in ['input','build','baseline','output','metric']:p.add_argument('--'+k,type=Path,required=True)
    p.add_argument('--frames',type=int,default=24);p.add_argument('--rounds',type=int,default=2)
    p.add_argument('--workers',type=int,default=10);p.add_argument('--budget',type=int,default=80000)
    p.add_argument('--full',action='store_true');a=p.parse_args();assert a.rounds>=2
    a.output.mkdir(parents=True,exist_ok=True);helper=a.build/'fajk_video_benchmark.exe';cli=a.build/'fajk_cli.exe'
    sha=lambda path:hashlib.file_digest(path.open('rb'),'sha256').hexdigest()
    report=dict(helper_sha256=sha(helper),cli_sha256=sha(cli),source_sha256=sha(a.input),
                frames=a.frames,workers=a.workers,budget=a.budget,records=[],failures=[],complete=False)
    save=lambda:(a.output/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    signatures={}
    for turn in range(a.rounds):
        variants=[(mode,be) for mode in ['quality','fast'] for be in ['cpu','gpu']]
        if turn%2:variants.reverse()
        for mode,backend in variants:
            prefix=a.output/f'{turn}-{mode}-{backend}'
            result=run([helper,a.input,prefix,a.frames,a.workers,a.budget,backend,0,mode])
            Path(str(prefix)+'.log').write_text(result['stdout'],encoding='utf-8')
            with Path(str(prefix)+'-summary.csv').open() as f:summary={k:float(v) for k,v in next(csv.DictReader(f)).items()}
            with prefix.with_suffix('.csv').open() as f:rows=list(csv.DictReader(f))
            packet_sha=sha(Path(str(prefix)+'-pipeline.fajkv'))
            signature=(packet_sha,tuple((r['pts'],r['duration'],r['decoded_crc']) for r in rows));key=(mode,backend)
            if key in signatures and signatures[key]!=signature:report['failures'].append(str(key)+': nondeterministic')
            signatures[key]=signature
            if mode=='quality' and packet_sha!=sha(a.baseline/f'0-after-{backend}-pipeline.fajkv'):
                report['failures'].append(backend+': quality packets changed')
            report['records'].append(dict(round=turn,mode=mode,backend=backend,packet_sha256=packet_sha,summary=summary));save()
            print(turn,mode,backend,'export',summary['export_ms'],'decode',summary['decode_ms'],flush=True)
    report['summary']={}
    for backend in ['cpu','gpu']:
        modes={}
        for mode in ['quality','fast']:
            rows=[r['summary'] for r in report['records'] if r['mode']==mode and r['backend']==backend]
            modes[mode]={k:statistics.mean(r[k] for r in rows) for k in ['export_ms','decode_ms','export_cpu_ms','decode_cpu_ms']}
        modes['fast_export_speedup']=modes['quality']['export_ms']/modes['fast']['export_ms'];report['summary'][backend]=modes
    # Quality metrics after the timed phase, never concurrently with benchmarks.
    report['quality_samples']=[]
    for backend in ['cpu','gpu']:
        for frame in [0,a.frames//2,a.frames-1]:
            prefix=a.output/f'0-quality-{backend}-frame{frame}'
            source=Path(str(prefix)+'-source.png');quality=Path(str(prefix)+'-decoded.png')
            fast=a.output/f'0-fast-{backend}-frame{frame}-decoded.png'
            q,f=map(float,subprocess.check_output([str(a.metric),str(source),str(quality),str(fast)],text=True).splitlines())
            report['quality_samples'].append(dict(backend=backend,frame=frame,quality_ss2=q,fast_ss2=f,delta=f-q));save()
    if a.full:
        exported=a.output/'full-fast-gpu.fajkv'
        report['full_export']=run([cli,'encode',a.input,exported,'--quality',100,'--target-bytes',a.budget,
                                   '--workers',a.workers,'--fast','--gpu','--overwrite'])
        report['full_sha256']=sha(exported);report['full_decoders']={};save()
        print('Full Fast export',report['full_export']['wall_ms'],'ms',flush=True)
        for backend in ['cpu','gpu']:
            prefix=a.output/f'full-fast-{backend}'
            validation=run([helper,'--decode',exported,prefix,a.workers,backend,a.input])
            with Path(str(prefix)+'-summary.csv').open() as f:summary={k:float(v) for k,v in next(csv.DictReader(f)).items()}
            with prefix.with_suffix('.csv').open() as f:rows=list(csv.DictReader(f))
            if any(int(r['bytes'])>a.budget for r in rows):report['failures'].append('Full Fast exceeded a frame budget')
            report['full_decoders'][backend]=dict(summary=summary,validation=validation['stdout']);save()
            print(backend,validation['stdout'].strip(),flush=True)
    report['complete']=True;save()
    if report['failures']:raise SystemExit('\n'.join(report['failures']))

if __name__=='__main__':main()
