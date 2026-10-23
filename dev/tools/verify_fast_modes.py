"""Alternate quality/Fast builds; measure speed, deterministic packets and source-relative quality.

Quality must match the archived R490 packets/pixels exactly on the same backend.
Fast may change coefficients but must preserve dimensions, budget, deterministic
output and GPU execution. Metrics run only after all performance measurements.
"""
import argparse,csv,hashlib,json,statistics,subprocess
from pathlib import Path
from PIL import Image

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for k in ['baseline','cli','output','ini','metric']:p.add_argument('--'+k,type=Path,required=True)
    p.add_argument('--inputs',nargs='+',type=Path,required=True)
    p.add_argument('--budgets',nargs='+',type=int,default=[80000])
    p.add_argument('--backends',nargs='+',choices=['cpu','gpu'],default=['gpu'])
    p.add_argument('--workers',type=int,default=10)
    p.add_argument('--repeats',type=int,default=4)
    p.add_argument('--rounds',type=int,default=2)
    a=p.parse_args();assert a.rounds>=2 and a.repeats>=2
    a.output.mkdir(parents=True,exist_ok=True)
    sha=lambda b:hashlib.sha256(b).hexdigest()
    report=dict(method=__doc__,baseline_sha256=sha(a.baseline.read_bytes()),cli_sha256=sha(a.cli.read_bytes()),
                ini_sha256=sha(a.ini.read_bytes()),workers=a.workers,repeats=a.repeats,rounds=a.rounds,
                cases=[],failures=[],complete=False)
    save=lambda:(a.output/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    for source in a.inputs:
        for budget in a.budgets:
            for backend in a.backends:
                tag=f'{source.stem}-{budget}-{backend}'
                case=dict(source=str(source),source_sha256=sha(source.read_bytes()),budget=budget,backend=backend,runs=[])
                for turn in range(a.rounds):
                    labels=['baseline','quality','fast'] if turn%2==0 else ['fast','quality','baseline']
                    for label in labels:
                        prefix=a.output/f'{tag}-{turn}-{label}'
                        cmd=[a.baseline if label=='baseline' else a.cli,'measure',source,prefix,100,a.repeats,
                             420,a.workers,100,'wavelet',budget-168,0,'r470-buffer',50,1,0,0,a.ini,'--'+backend]
                        if label!='baseline':cmd+=['--fast' if label=='fast' else '--more-quality']
                        result=subprocess.run(list(map(str,cmd)),text=True,capture_output=True)
                        (prefix.with_suffix('.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
                        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
                        rows=list(csv.DictReader(prefix.with_suffix('.csv').open()))
                        with Image.open(prefix.with_suffix('.png')) as im:
                            pixels=sha(im.convert('RGBA').tobytes());size=im.size
                        with Image.open(source) as im:assert size==im.size,'Dimensions changed'
                        packet=prefix.with_suffix('.fajkv').read_bytes()
                        if len(packet)>budget:report['failures'].append(tag+': exceeded budget')
                        if any(int(r['gpu_fallbacks']) for r in rows):report['failures'].append(tag+': GPU fallback')
                        if backend=='gpu' and not all(int(r['gpu_transforms']) for r in rows):report['failures'].append(tag+': missing GPU work')
                        case['runs'].append(dict(mode=label,round=turn,packet=sha(packet),pixels=pixels,bytes=len(packet),
                            **{k:statistics.mean(float(r[k]) for r in rows) for k in ['encode_ms','decode_ms','encode_cycles','decode_cycles']}))
                for label in ['baseline','quality','fast']:
                    runs=[r for r in case['runs'] if r['mode']==label]
                    case[label]={k:statistics.mean(r[k] for r in runs) for k in ['encode_ms','decode_ms','encode_cycles','decode_cycles','bytes']}
                    if len({(r['packet'],r['pixels']) for r in runs})!=1:report['failures'].append(tag+': nondeterministic '+label)
                quality=[r for r in case['runs'] if r['mode']!='fast']
                case['quality_exact']=len({(r['packet'],r['pixels']) for r in quality})==1
                if not case['quality_exact']:report['failures'].append(tag+': quality baseline changed')
                case['fast_encode_speedup']=case['quality']['encode_ms']/case['fast']['encode_ms']
                report['cases'].append(case);save()
                print(tag,'Fast speedup',round(case['fast_encode_speedup'],3),'quality exact',case['quality_exact'],flush=True)
    # Source-relative metric is intentionally outside the timed phase.
    for case in report['cases']:
        tag=f"{Path(case['source']).stem}-{case['budget']}-{case['backend']}"
        paths=[a.output/f'{tag}-0-{label}.png' for label in ['quality','fast']]
        values=subprocess.check_output([str(a.metric),case['source'],*map(str,paths)],text=True).splitlines()
        q,f=map(float,values);case['quality']['ssimulacra2']=q;case['fast']['ssimulacra2']=f
        case['fast_ss2_delta']=f-q;save()
        print(tag,'SS2 quality',q,'fast',f,'delta',round(f-q,6),flush=True)
    report['complete']=True;save()
    if report['failures']:raise SystemExit('\n'.join(report['failures']))

if __name__=='__main__':main()
