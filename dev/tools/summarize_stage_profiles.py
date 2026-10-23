"""Average per-frame wall-time profiles without double-counting nested timers.

Use --profiles with CLI/video *-stages.csv files from one workload/backend.
Repeated events (rate-search attempts) are summed per frame before averaging.
Missing stages count as zero. CPU/GPU timers include host work and waits.
"""
import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profiles', nargs='+', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    frames = []
    totals = []
    for path in a.profiles:
        grouped = defaultdict(lambda: defaultdict(float))
        with path.open(newline='') as stream:
            for row in csv.DictReader(stream):
                grouped[row.get('repeat', row.get('frame'))][row['stage']] += float(row['ms'])
        frames.extend(grouped.values())
        companion = path.with_name(path.name.replace('-stages.csv', '.csv'))
        if companion.exists():
            with companion.open(newline='') as stream:
                totals.extend(dict(encode_ms=float(r['encode_ms']), decode_ms=float(r['decode_ms']))
                              for r in csv.DictReader(stream))
    stages = {stage: statistics.mean(frame.get(stage, 0) for frame in frames)
              for stage in sorted(set().union(*(frame.keys() for frame in frames)))}
    nested = lambda s: (s.startswith('encode.detail.') or s.startswith('encode.work.') or s.startswith('encode.pack_')
                        or s in {'decode.fast441.snapshot', 'decode.fast441.recurrence',
                                 'decode.fast441.text', 'decode.fast441.output_allocation'})
    report = dict(method=__doc__, profiles=[str(p) for p in a.profiles], frames=len(frames),
                  stages_ms=stages, nested_stages=[s for s in stages if nested(s)])
    report['accounting'] = {}
    for operation in ['encode', 'decode']:
        measured = sum(v for k, v in stages.items() if k.startswith(operation + '.') and not nested(k))
        entry = dict(exclusive_stages_ms=measured)
        if totals:
            entry['total_ms'] = statistics.mean(r[operation + '_ms'] for r in totals)
            entry['outside_stage_timers_ms'] = entry['total_ms'] - measured
        report['accounting'][operation] = entry
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
