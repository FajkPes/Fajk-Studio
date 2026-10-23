"""Compare contour confidence and actual render weights on identical native pixels.

Create prefixes with `fajk_cli inspect-contours INPUT PREFIX 8 [baseline]`.
Green is the real contour blend weight, red is the colored side-cleanup weight.
No mask dilation, normalization, or artificial smoothing is performed here.
"""
import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


ANALYSIS_REASONS = {
    0: 'model evaluated', 1: 'nonopaque', 2: 'uniform', 3: 'planar/shallow',
    4: 'insufficient span', 5: 'too few samples on one side',
    6: 'insufficient color energy', 7: 'no oriented transition',
}
LOCATION_REASONS = {
    0: 'localized', 1: 'weak color model', 2: 'weak normal', 3: 'weak phase range',
    4: 'too few unique crossings', 5: 'inconsistent slope', 6: 'inconsistent positions',
}


def read(prefix):
    native = np.asarray(Image.open(str(prefix) + '-native.png').convert('RGB'))
    effect = np.asarray(Image.open(str(prefix) + '-effect.png').convert('RGB'))
    with open(str(prefix) + '-guides.csv', newline='') as stream:
        guides = list(csv.DictReader(stream))
    if native.shape != effect.shape:
        raise ValueError('Native and effect dimensions differ.')
    return native, effect, guides


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--box', nargs=4, type=int, help='Native working-image coordinates.')
    parser.add_argument('--scale', type=int, default=1)
    args = parser.parse_args()
    if not 1 <= args.scale <= 4:
        parser.error('Scale must be 1..4.')
    old_native, old_effect, old_guides = read(args.before)
    native, effect, guides = read(args.after)
    if not np.array_equal(old_native, native):
        parser.error('Native pixels differ: this is not an isolated contour comparison.')
    if [(r['x'], r['y']) for r in old_guides] != [(r['x'], r['y']) for r in guides]:
        parser.error('Guide grids differ.')
    height, width = native.shape[:2]
    box = args.box or (0, 0, width, height)
    x0, y0, x1, y1 = box
    if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
        parser.error('Crop is outside the native working image.')
    args.output.mkdir(parents=True, exist_ok=True)
    summary = {'native_rgb_sha256': hashlib.sha256(native.tobytes()).hexdigest(), 'box': box}
    for name, old, new in [('whole', old_effect[:, :, 1], effect[:, :, 1]),
                           ('crop', old_effect[y0:y1, x0:x1, 1], effect[y0:y1, x0:x1, 1])]:
        active_old, active_new = old >= 8, new >= 8
        summary[name] = {
            'contour_weight_threshold': 8, 'before_pixels': int(active_old.sum()),
            'after_pixels': int(active_new.sum()),
            'new_pixels': int((~active_old & active_new).sum()),
            'lost_pixels': int((active_old & ~active_new).sum()),
        }
    for name, rows in [('before', old_guides), ('after', guides)]:
        missing = [r for r in rows if float(r['geometry']) <= .05]
        summary[name] = {'guides': len(rows), 'geometry_above_005': len(rows) - len(missing)}
        for column, labels in [('broad_reason', ANALYSIS_REASONS), ('broad_location_reason', LOCATION_REASONS)]:
            if missing and column in missing[0]:
                counts = Counter(int(r[column]) for r in missing)
                summary[name][column] = {labels.get(code, str(code)): count for code, count in counts.items()}
    summary['previously_usable_guides_changed'] = sum(
        float(old['geometry']) >= .03 and any(old[k] != new[k] for k in ('color', 'geometry', 'thin'))
        for old, new in zip(old_guides, guides)
    )
    images = [native[y0:y1, x0:x1], old_effect[y0:y1, x0:x1], effect[y0:y1, x0:x1]]
    panel_width, panel_height = (x1 - x0) * args.scale, (y1 - y0) * args.scale
    canvas = Image.new('RGB', (panel_width * 3, panel_height + 44), (18, 18, 18))
    draw = ImageDraw.Draw(canvas)
    for index, (pixels, title) in enumerate(zip(images, ['NATIVE / fixed input', 'BEFORE / actual weights', 'AFTER / actual weights'])):
        canvas.paste(Image.fromarray(pixels).resize((panel_width, panel_height), Image.Resampling.NEAREST), (index * panel_width, 44))
        draw.text((index * panel_width + 6, 14), title, fill='white')
    canvas.save(args.output / 'comparison.png')
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
