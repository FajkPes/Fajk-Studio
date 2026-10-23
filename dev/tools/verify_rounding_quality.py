"""Assess bounded GPU rounding changes against the original source using SSIMULACRA2.

Run after performance tests. Reads verify_exact_optimization.py output; does not
relax packet equality or pixel bounds, and measures every changed photo case.
"""
import argparse
import json
import subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--results',type=Path,required=True)
    p.add_argument('--metric',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    report=json.loads(a.results.read_text(encoding='utf-8'))
    assert report['complete'] and not report['failures']
    records=[];failures=[]
    for case in report['cases']:
        assert case['packets_exact'] and case['pixels_within_bound']
        if case['pixels_exact']:
            records.append(dict(source=case['source'],budget=case['budget'],identical_pixels=True))
            continue
        tag=f"{Path(case['source']).stem}-{case['budget']}-{case['backend']}"
        paths=[a.results.parent/f'{tag}-0-{label}.png' for label in ['before','after']]
        scores=subprocess.check_output([str(a.metric),str(case['source']),*map(str,paths)],text=True).splitlines()
        before,after=map(float,scores)
        record=dict(source=case['source'],budget=case['budget'],identical_pixels=False,
                    ss2_before=before,ss2_after=after,ss2_delta=after-before,
                    pixel_maximum=max(r['pixel_maximum'] for r in case['runs']),
                    pixel_mean=max(r['pixel_mean'] for r in case['runs']))
        records.append(record)
        # The source-relative quality must not regress. A larger positive
        # delta is an improvement, while the independent pixel bound still
        # limits any reconstruction change in either direction.
        if after-before<-.001:failures.append(record)
        print(case['budget'],Path(case['source']).name,'SS2 delta',after-before,flush=True)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(dict(complete=True,records=records,failures=failures),indent=2),encoding='utf-8')
    if failures:raise SystemExit('Source-relative quality regression: '+str(failures))
    print('PASS original-source quality for',len(records),'bounded rounding cases',flush=True)


if __name__=='__main__':main()
