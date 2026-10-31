"""Alternate R470 CPU/GPU end-to-end runs; check quality, budgets, CPU work and cross-decode.

Needs numpy/Pillow and the existing fajk_metric. Times exclude process startup,
shader warmup and PNG IO. All transfers, driver waiting and CPU filters are timed.
Process CPU time sums all codec/driver threads; it is not wall time or energy.
The decoder gate permits isolated differences <=16 in final RGBA because later
CPU contour/text decisions can amplify a one-level rounding change. Mean error
must be <=.001/255 and <=.2% of byte channels may differ; SS2 loss must be <=.2.
Raw GPU transform/filter tests apply tighter local error limits.
"""
import argparse,csv,json,statistics,subprocess,hashlib
from pathlib import Path
import numpy as np
from PIL import Image

def run(command):
    result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    return result.stdout

def difference(a,b):
    x=np.asarray(Image.open(a).convert('RGBA'),dtype=np.int16)
    y=np.asarray(Image.open(b).convert('RGBA'),dtype=np.int16)
    d=np.abs(x-y)
    return dict(maximum=int(d.max()),mean=float(d.mean()),changed_fraction=float(np.mean(d!=0)))

def main():
    dev=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cli',type=Path,default=dev/'build-r4/fajk_cli.exe')
    p.add_argument('--metric',type=Path,default=dev/'build-r4/fajk_metric.exe')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--inputs',type=Path,nargs='+',default=[dev/'samples/references'/f'{n}-reference.png' for n in ['22920','22950','23006']])
    p.add_argument('--budgets',type=int,nargs='+',default=[30000,80000,150000])
    p.add_argument('--repeats',type=int,default=12);p.add_argument('--rounds',type=int,default=2)
    p.add_argument('--workers',type=int,default=10)
    p.add_argument('--reuse',action='store_true',help='Recheck saved measurements and pixels without rerunning timings.')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    report=dict(method=__doc__,cli_sha256=hashlib.sha256(a.cli.read_bytes()).hexdigest(),hardware=run([a.cli,'hardware']),workers=a.workers,repeats=a.repeats,rounds=a.rounds,cases=[])
    failures=[]
    for source in a.inputs:
        for budget in a.budgets:
            tag=f'{source.stem}-{budget}';samples={'cpu':[],'gpu':[]};last={}
            for round_id in range(a.rounds):
                for backend in (['cpu','gpu'] if round_id%2==0 else ['gpu','cpu']):
                    prefix=a.output/f'{tag}-{round_id}-{backend}'
                    if not (a.reuse and prefix.with_suffix('.csv').exists()):run([a.cli,'measure',source,prefix,100,a.repeats,420,a.workers,100,'wavelet',budget-168,0,'r470-buffer',50,1,0,0,f'--{backend}'])
                    samples[backend].extend(dict(r) for r in csv.DictReader(prefix.with_suffix('.csv').open()))
                    last[backend]=prefix
            case=dict(source=str(source),source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),budget=budget,backends={})
            for backend,rows in samples.items():
                case['backends'][backend]={k:statistics.median(float(r[k]) for r in rows) for k in ['encode_ms','decode_ms','encode_cpu_ms','decode_cpu_ms','encode_cycles','decode_cycles']}
                case['backends'][backend]['total_encode_cpu_ms']=sum(float(r['encode_cpu_ms']) for r in rows)
                case['backends'][backend]['total_decode_cpu_ms']=sum(float(r['decode_cpu_ms']) for r in rows)
                case['backends'][backend]['frames']=len(rows)
                for key in ['encode_cpu_ms','decode_cpu_ms']:
                    case['backends'][backend][key]=statistics.mean(float(r[key]) for r in rows)
                for key in ['encode_ms','decode_ms']:
                    case['backends'][backend][key.replace('_ms','_p95_ms')]=float(np.percentile([float(r[key]) for r in rows],95))
                case['backends'][backend]['file_bytes']=last[backend].with_suffix('.fajkv').stat().st_size
                case['backends'][backend]['gpu_fallbacks']=sum(int(r['gpu_fallbacks']) for r in rows)
                if any(int(r['bytes'])+168>budget for r in rows):failures.append(tag+': budget exceeded')
                if backend=='gpu' and not any(int(r['gpu_transforms'])+int(r['gpu_resamples']) for r in rows):failures.append(tag+': no GPU work')
            cpu_packet_gpu=a.output/f'{tag}-cpu-packet-gpu.png';gpu_packet_cpu=a.output/f'{tag}-gpu-packet-cpu.png'
            run([a.cli,'decode',last['cpu'].with_suffix('.fajkv'),0,cpu_packet_gpu,'--gpu'])
            run([a.cli,'decode',last['gpu'].with_suffix('.fajkv'),0,gpu_packet_cpu,'--cpu'])
            case['cpu_packet_cross_decode']=difference(last['cpu'].with_suffix('.png'),cpu_packet_gpu)
            case['gpu_packet_cross_decode']=difference(last['gpu'].with_suffix('.png'),gpu_packet_cpu)
            for key in ['cpu_packet_cross_decode','gpu_packet_cross_decode']:
                if case[key]['maximum']>16 or case[key]['mean']>.001 or case[key]['changed_fraction']>.002:failures.append(tag+': decoder mismatch '+key)
            score=run([a.metric,source,last['cpu'].with_suffix('.png'),last['gpu'].with_suffix('.png')])
            case['ssimulacra2_cpu'],case['ssimulacra2_gpu']=map(float,score.strip().splitlines())
            case['ssimulacra2_delta']=case['ssimulacra2_gpu']-case['ssimulacra2_cpu']
            if case['ssimulacra2_delta']<-.2:failures.append(tag+': quality regression')
            for key in ['encode_ms','decode_ms','encode_cpu_ms','decode_cpu_ms']:
                cpu=case['backends']['cpu'][key];gpu=case['backends']['gpu'][key]
                case[key+'_ratio']=gpu/cpu if cpu else None
            report['cases'].append(case);report['failures']=failures
            (a.output/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
            print(tag,'encode',round(case['encode_ms_ratio'],3),'decode',round(case['decode_ms_ratio'],3),'SS2 delta',round(case['ssimulacra2_delta'],5),flush=True)
    if failures:raise SystemExit('\n'.join(failures))

if __name__=='__main__':main()
