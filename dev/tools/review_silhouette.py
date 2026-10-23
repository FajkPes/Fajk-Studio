"""Inspect both sides of a known silhouette using an original-only color mask.

This is a scene-specific diagnostic, not an automatic universal quality metric.
The mask is identical for before/after and is exported for inspection. Images
are actual decoded pixels, with nearest-neighbor enlargement for the detail.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('reference', 'before', 'after', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--box', nargs=4, type=int, required=True)
    parser.add_argument('--background-color', nargs=3, type=int, required=True)
    parser.add_argument('--tolerance', type=float, default=80)
    args = parser.parse_args()
    full = [Image.open(path).convert('RGB') for path in (args.reference, args.before, args.after)]
    if any(image.size != full[0].size for image in full):
        parser.error('All three images must have identical dimensions.')
    x0, y0, x1, y1 = args.box
    if not (0 <= x0 < x1 <= full[0].width and 0 <= y0 < y1 <= full[0].height):
        parser.error('The crop must lie inside the image.')
    if args.tolerance <= 0 or any(c < 0 or c > 255 for c in args.background_color):
        parser.error('Invalid background color or tolerance.')
    args.output.mkdir(parents=True, exist_ok=True)
    crops = [image.crop(args.box) for image in full]
    reference = np.asarray(crops[0]).astype(float)
    background = np.linalg.norm(reference - np.array(args.background_color), axis=2) < args.tolerance
    records = []
    mask_preview = np.zeros((*background.shape, 3), dtype=np.uint8)
    for radius in (3, 8, 16):
        def dilate(mask):
            return np.asarray(Image.fromarray(mask.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(2 * radius + 1))) > 0
        for label, mask in [('background', background & dilate(~background)),
                            ('foreground', ~background & dilate(background))]:
            record = dict(side=label, radius=radius, pixels=int(mask.sum()))
            if not mask.any():
                continue
            for name, image in zip(('before', 'after'), crops[1:]):
                error = np.asarray(image).astype(float) - reference
                record[name + '_mae'] = float(np.abs(error[mask]).mean())
                record[name + '_rmse'] = float(np.sqrt(np.square(error[mask]).mean()))
            records.append(record)
            if radius == 8:
                mask_preview[mask] = (70, 190, 255) if label == 'background' else (255, 190, 60)
    Image.fromarray(mask_preview).save(args.output / 'mask.png')
    width, height = crops[0].size
    comparison = Image.new('RGB', (width * 6, height * 2 + 40), (18, 18, 18))
    draw = ImageDraw.Draw(comparison)
    for i, (name, crop) in enumerate(zip(('ORIGINAL', 'BEFORE', 'AFTER'), crops)):
        comparison.paste(crop.resize((width * 2, height * 2), Image.Resampling.NEAREST), (i * width * 2, 40))
        draw.text((i * width * 2 + 8, 13), name, fill='white')
    comparison.save(args.output / 'comparison.png')
    result = dict(box=args.box, background_color=args.background_color, tolerance=args.tolerance, bands=records)
    (args.output / 'results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
