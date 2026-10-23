"""Check R500 CLI aliases and keep legacy numeric AQ strength unambiguous."""
import argparse, json, subprocess
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ['cli','input','output']:parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    def run(*command):return subprocess.check_output([str(args.cli),*map(str,command)],text=True)
    assert 'R500 CPU' in run('encoders') and 'R500 GPU' in run('encoders')
    assert 'R500' in run() and '--enable-aq' in run()
    packets={}
    for label,options in [('default',[]),('off',['--no-aq']),('strength',['--aq','250']),('enabled',['--enable-aq']),('enabled-strength',['--enable-aq','--aq','100']),('on-off',['--enable-aq','--no-aq']),('off-on',['--no-aq','--enable-aq']),('gpu-alias',['--r500-gpu']),('gpu',['--gpu'])]:
        path=args.output/(label+'.fajk')
        run('encode',args.input,path,'--fast','--workers','2','--422','--target-bytes','80000','--overwrite',*options)
        packets[label]=path.read_bytes()
    assert packets['default']==packets['off']==packets['strength']==packets['on-off']
    assert packets['enabled']==packets['enabled-strength']==packets['off-on']
    assert packets['enabled']!=packets['off'],'AQ enable did not change the controlled fixture'
    assert packets['gpu-alias']==packets['gpu']
    for profile in ['r500','r500-buffer','r500-gpu','r500-gpu-buffer']:
        run('measure',args.input,args.output/profile,'10','1','422','2','100','wavelet','80000','0',profile,'--fast')
    (args.output/'results.json').write_text(json.dumps(dict(complete=True,aq_numeric_compatibility=True,default_off=True,ordered_master_switches=True,gpu_alias=True,measure_profiles=4),indent=2),encoding='utf-8')
    print('PASS R500 CLI labels, legacy numeric AQ, master switches, GPU alias and four measurement profiles')

if __name__=='__main__':main()
