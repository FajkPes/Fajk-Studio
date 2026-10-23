"""Review real flat-area bit allocation at fixed whole-container byte limits.

Diagnostic tile bits are distributed over transform footprints, not exact
per-pixel bit ownership. Fixed source-space ROIs make comparisons independent
of the changing eligibility map. Diagnostic encode timings are not benchmarks.
"""
import argparse
import csv
import json
import subprocess
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from compare_decode import assess_quality

ROIS = {
    '22950': {'wall_left': (220, 220, 920, 650),
              'wall_right': (1660, 180, 2250, 800),
              'car': (650, 730, 1660, 1300),
              'floor': (420, 1310, 2000, 1420),
              'ui_text': (30, 1080, 380, 1140)},
    '23006': {'wall_left': (110, 240, 530, 680),
              'wall_right': (1880, 380, 2300, 700),
              'character': (900, 620, 1580, 1380),
              'window': (650, 30, 1250, 550),
              'tutorial': (20, 55, 520, 200)},
    '22920': {'background': (1720, 100, 1880, 480),
              'face': (1980, 130, 2070, 245),
              'hair': (1935, 150, 1978, 265),
              'cap': (1925, 65, 2080, 145),
              'text': (2090, 155, 2400, 275)},
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--samples', nargs='+', default=['23006', '22920'])
    p.add_argument('--budgets', type=int, nargs='+', default=[30000, 80000])
    p.add_argument('--settings', type=int, nargs='+', default=[0, 50, 75, 100])
    p.add_argument('--compare-zero', action='store_true', help='Diagnostic counterfactual at control 0; never part of normal codec timing.')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    dev = Path(__file__).resolve().parents[1]
    results = []
    for sample in a.samples:
        ref = dev / 'samples/references' / (sample + '-reference.png')
        if not ref.exists():
            ref = dev / 'samples/references' / (sample + '.png')
        source = np.asarray(Image.open(ref).convert('RGB')).astype(np.int16)
        sh, sw = source.shape[:2]
        for budget in a.budgets:
            images = []
            for setting in a.settings:
                prefix = a.output / f'{sample}-{budget}-flat{setting}'
                subprocess.run([str(a.exe.resolve()), 'inspect-allocation', str(ref),
                                str(prefix), str(budget - 168), str(setting)] + (['compare0'] if a.compare_zero else []), check=True)
                metadata = json.loads(Path(str(prefix) + '-allocation.json').read_text())
                assert prefix.with_suffix('.fajk').stat().st_size <= budget
                with Path(str(prefix) + '-allocation.csv').open() as f:
                    cells = list(csv.DictReader(f))
                tiles_path = Path(str(prefix) + '-tiles.csv')
                if tiles_path.exists():
                    with tiles_path.open() as f:
                        tiles = list(csv.DictReader(f))
                else:
                    tiles = None
                decoded = np.asarray(Image.open(prefix.with_suffix('.png')).convert('RGB')).astype(np.int16)
                row = dict(sample=sample, budget=budget, setting=setting,
                           file_bytes=prefix.with_suffix('.fajk').stat().st_size,
                           detail_bits=sum(float(c['detail_bits']) for c in cells), rois={})
                if a.compare_zero and Path(str(prefix) + '-savings.json').exists():
                    row['flat_savings'] = json.loads(Path(str(prefix) + '-savings.json').read_text())
                row.update(assess_quality(ref, prefix.with_suffix('.png'), prefix.with_suffix('.png'),
                                         dev / 'build-r4/fajk_metric.exe'))
                for name, (x0, y0, x1, y1) in ROIS.get(sample, {}).items():
                    chosen = [c for c in cells if x0 <= (int(c['cell_x'])+.5)*metadata['cell']*sw/metadata['working_width'] < x1
                              and y0 <= (int(c['cell_y'])+.5)*metadata['cell']*sh/metadata['working_height'] < y1]
                    row['rois'][name] = dict(
                        rgb_mae=float(np.abs(source[y0:y1, x0:x1]-decoded[y0:y1, x0:x1]).mean()),
                        distributed_detail_bits=sum(float(c['detail_bits']) for c in chosen),
                        mean_y_penalty=float(np.mean([float(c['y_penalty']) for c in chosen])) if chosen else None,
                        mean_flat_eligibility=float(np.mean([float(c['flat_eligibility']) for c in chosen])) if chosen else None)
                    if tiles is not None:
                        # Entire tile footprints inside this fixed source ROI;
                        # no redistribution of a distant edge's tile cost.
                        sx, sy = sw/metadata['working_width'], sh/metadata['working_height']
                        local = [t for t in tiles if x0 <= int(t['x'])*sx and y0 <= int(t['y'])*sy
                                 and (int(t['x'])+int(t['width']))*sx <= x1
                                 and (int(t['y'])+int(t['height']))*sy <= y1]
                        row['rois'][name]['contained_tile_bits_by_level'] = {
                            str(level): sum(float(t['detail_bits']) for t in local if int(t['level'])==level)
                            for level in range(6)}
                results.append(row)
                (a.output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
                print(json.dumps(row), flush=True)
                crop = {'23006': (1070, 700, 1800, 1350), '22950': (650, 730, 1660, 1300)}.get(sample, (1900, 65, 2340, 520))
                im = Image.open(prefix.with_suffix('.png')).convert('RGB').crop(crop)
                images.append((setting, im))
            w, h = images[0][1].size
            review = Image.new('RGB', (w*len(images), h+32), '#111111')
            draw = ImageDraw.Draw(review)
            for i, (setting, im) in enumerate(images):
                review.paste(im, (i*w, 32))
                draw.text((i*w+8, 9), f'FLAT {setting}% / {budget} B', fill='white')
            review.save(a.output / f'{sample}-{budget}-review.png')


if __name__ == '__main__':
    main()
