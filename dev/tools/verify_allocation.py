"""Exercise the allocation slider on a real source, with spatial bit accounting.

The ROI costs are attributed entropy costs, including every detail band. They
are not independently removable byte ranges. Coarse bands overlap multiple
regions. The default ROIs below are specific to screenshot 23006, 2560x1440.
Use the generated PNGs for visual review; a bit saving alone is not a quality win.
"""
import argparse
import csv
import json
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from compare_decode import assess_quality


def read_map(prefix):
    geometry = json.loads(Path(str(prefix) + '-allocation.json').read_text())
    with Path(str(prefix) + '-allocation.csv').open() as stream:
        rows = [{key: float(value) for key, value in row.items()}
                for row in csv.DictReader(stream)]
    return geometry, rows


def roi_bits(geometry, rows, box):
    x0, y0, x1, y1 = box
    sx = geometry['source_width'] / geometry['working_width']
    sy = geometry['source_height'] / geometry['working_height']
    cell = geometry['cell']
    total = 0.
    for row in rows:
        x, y = row['cell_x'] * cell, row['cell_y'] * cell
        left, top = x * sx, y * sy
        right = min(geometry['working_width'], x + cell) * sx
        bottom = min(geometry['working_height'], y + cell) * sy
        area = max(0., min(right, x1) - max(left, x0)) * max(0., min(bottom, y1) - max(top, y0))
        total += row['detail_bits'] * area / ((right - left) * (bottom - top))
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--baseline-cli', type=Path)
    parser.add_argument('--metric', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = np.asarray(Image.open(args.input).convert('RGBA'))
    if source.shape[:2] != (1440, 2560):
        parser.error('Default ROIs require screenshot 23006 at 2560x1440.')
    rois = {'left_wall': (300, 280, 620, 520), 'right_wall': (2000, 450, 2300, 750),
            'character_and_box': (900, 620, 1580, 1380), 'tutorial_text': (20, 55, 520, 200)}
    report = []
    font = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 18)
    with (args.output / 'runs.log').open('w', encoding='utf-8') as log:
        for budget in (30000, 80000, 150000):
            values = (0, 25, 50, 75, 100) if budget == 80000 else (0, 50, 100)
            baseline = args.output / f'{budget}-flat50'
            # Always produce baseline first, so metrics use exactly the same encode.
            ordered = (50,) + tuple(value for value in values if value != 50)
            maps = {}
            for value in ordered:
                prefix = args.output / f'{budget}-flat{value}'
                subprocess.run([str(args.cli.resolve()), 'inspect-allocation', str(args.input.resolve()),
                                str(prefix.resolve()), str(budget - 168), str(value)],
                               stdout=log, stderr=subprocess.STDOUT, check=True)
                geometry, rows = read_map(prefix)
                maps[value] = geometry, rows
                size = Path(str(prefix) + '.fajk').stat().st_size
                assert size <= budget, (budget, value, size)
                decoded = np.asarray(Image.open(str(prefix) + '.png').convert('RGBA'))
                assert decoded.shape == source.shape and np.all(decoded[:, :, 3] == 255)
                record = {'budget': budget, 'slider': value, 'file_bytes': size,
                          'white_percent': 100 * sum(row['flat_eligibility'] >= .999 for row in rows) / len(rows),
                          'rois': {name: roi_bits(geometry, rows, box) for name, box in rois.items()}}
                if args.metric:
                    record['quality'] = assess_quality(args.input.resolve(), Path(str(baseline) + '.png'),
                                                       Path(str(prefix) + '.png'), args.metric.resolve())
                if value == 50 and args.baseline_cli:
                    previous = args.output / f'{budget}-R461'
                    subprocess.run([str(args.baseline_cli.resolve()), 'measure', str(args.input.resolve()),
                                    str(previous.resolve()), '100', '1', '420', '0', '100', 'wavelet',
                                    str(budget - 168), '0', 'r461-buffer'], stdout=log,
                                   stderr=subprocess.STDOUT, check=True)
                    old_bytes = Path(str(previous) + '.fajkv').read_bytes()
                    # The single image container deliberately has a different
                    # fps header; compare via a new measure container with same fps.
                    current = args.output / f'{budget}-R462'
                    subprocess.run([str(args.cli.resolve()), 'measure', str(args.input.resolve()),
                                    str(current.resolve()), '100', '1', '420', '0', '100', 'wavelet',
                                    str(budget - 168), '0', 'r462-buffer', '50'], stdout=log,
                                   stderr=subprocess.STDOUT, check=True)
                    record['baseline_bytes_identical'] = old_bytes == Path(str(current) + '.fajkv').read_bytes()
                    record['baseline_pixels_identical'] = bool(np.array_equal(
                        np.asarray(Image.open(str(previous) + '.png').convert('RGBA')), decoded))
                    assert record['baseline_bytes_identical'] and record['baseline_pixels_identical']
                report.append(record)
                (args.output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            # Source-sized working grids must not change with the slider, and
            # eligibility must monotonically expand. Actual entropy costs need
            # not be monotonic because the common rate search redistributes bits.
            for low, high in zip(values, values[1:]):
                lg, lr = maps[low]
                hg, hr = maps[high]
                assert (lg['working_width'], lg['working_height']) == (hg['working_width'], hg['working_height'])
                assert len(lr) == len(hr)
                assert all(a['flat_eligibility'] <= b['flat_eligibility'] for a, b in zip(lr, hr))
            canvas = Image.new('RGB', (400 * len(values), 515), '#141414')
            draw = ImageDraw.Draw(canvas)
            for col, value in enumerate(values):
                prefix = args.output / f'{budget}-flat{value}'
                draw.text((400 * col + 10, 4), f'{value} % | {budget // 1000} kB', font=font, fill='white')
                flat = Image.open(str(prefix) + '-flat.png').convert('RGB')
                canvas.paste(flat.resize((400, 225), Image.Resampling.NEAREST), (400 * col, 35))
                rgb = Image.open(str(prefix) + '.png').convert('RGB')
                rgb.thumbnail((400, 225), Image.Resampling.LANCZOS)
                canvas.paste(rgb, (400 * col, 280))
            canvas.save(args.output / f'{budget}-review.png')
            for label, box in {'character': (1070, 700, 1800, 1350),
                               'wall': (300, 280, 620, 520), 'text': (20, 55, 520, 200)}.items():
                w, h = box[2] - box[0], box[3] - box[1]
                crop = Image.new('RGB', (w * 4, h + 40), '#141414')
                draw = ImageDraw.Draw(crop)
                for col, (caption, path) in enumerate([
                        ('Original', args.input), *[(f'{value} %', args.output / f'{budget}-flat{value}.png')
                                                   for value in (0, 50, 100)]]):
                    draw.text((col * w + 10, 4), caption, font=font, fill='white')
                    crop.paste(Image.open(path).crop(box), (col * w, 40))
                crop.save(args.output / f'{budget}-{label}.png')
    print('Budget limits, monotonic maps, image dimensions, opacity and R461 baseline PASS.')
    for budget in (30000, 80000, 150000):
        base = next(row for row in report if row['budget'] == budget and row['slider'] == 50)
        high = next(row for row in report if row['budget'] == budget and row['slider'] == 100)
        savings = {key: round(100 * (1 - high['rois'][key] / base['rois'][key]), 2)
                   for key in ('left_wall', 'right_wall')}
        print(budget, '100 vs 50: attributed wall savings %', savings)


if __name__ == '__main__':
    main()
