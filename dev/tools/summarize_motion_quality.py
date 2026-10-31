"""Summarize the complete motion_quality_probe CSV without averaging RMS values."""
import argparse
import csv
import json
import math
from pathlib import Path


def summarize(path):
    rows = list(csv.DictReader(path.open(newline='', encoding='utf-8-sig')))
    if not rows:
        raise ValueError('Empty quality CSV')
    for index, row in enumerate(rows):
        if int(row['frame']) != index:
            raise ValueError('Incomplete or unordered quality CSV')
        for key, value in row.items():
            if not math.isfinite(float(value)):
                raise ValueError(f'Non-finite {key} at frame {index}')

    def mean(key):
        return sum(float(row[key]) for row in rows) / len(rows)

    def weighted_rms(key, weight):
        samples = sum(int(row[weight]) for row in rows)
        return math.sqrt(sum(float(row[key]) ** 2 * int(row[weight]) for row in rows) / samples) if samples else None

    result = dict(frames=len(rows))
    for suffix, label in [('intra', 'baseline'), ('max', 'candidate')]:
        result[label] = dict(packet_bytes=sum(int(row[f'{suffix}_bytes']) for row in rows),
            rgb_mse=mean(f'mse_{suffix}'), chroma_mse=mean(f'chroma_mse_{suffix}'),
            chroma_error_gradient_rms=math.sqrt(mean_square(rows, f'chroma_error_gradient_{suffix}')),
            stable_flicker_rms=weighted_rms(f'flicker_{suffix}', 'stable_samples'),
            motion_flicker_rms=weighted_rms(f'motion_flicker_{suffix}', 'motion_stable_samples'))
    return result


def mean_square(rows, key):
    return sum(float(row[key]) ** 2 for row in rows) / len(rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    text = json.dumps(summarize(args.csv), indent=2)
    if args.output:
        args.output.write_text(text + '\n', encoding='utf-8')
    print(text)
