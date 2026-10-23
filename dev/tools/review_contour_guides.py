"""Render actual contour confidence, separately from rounded pixel differences.

First run: fajk_cli inspect-contours INPUT.fajk PREFIX 8
Then run this tool with PREFIX and --box in native working-image coordinates.
These are model confidences, not object segmentation or an effect-strength map.
"""
import argparse
import csv
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    parser.add_argument('--box', nargs=4, type=int)
    parser.add_argument('--scale', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.scale <= 8:
        parser.error('Scale must be 1..8.')
    native = Image.open(str(args.prefix) + '-native.png').convert('RGB')
    with open(str(args.prefix) + '-guides.csv', newline='') as stream:
        rows = list(csv.DictReader(stream))
    xs = sorted({int(row['x']) for row in rows})
    ys = sorted({int(row['y']) for row in rows})
    if not xs or not ys:
        parser.error('Empty guide report.')
    lookup = {(int(row['x']), int(row['y'])): row for row in rows}
    box = args.box or (0, 0, native.width, native.height)
    if not (0 <= box[0] < box[2] <= native.width and 0 <= box[1] < box[3] <= native.height):
        parser.error('Crop is outside the native working image.')
    images = [native.crop(box)]
    titles = ['NATIVE / before contour cleanup']
    # Match the renderer's clamped, bilinearly interpolated scalar confidence.
    # No blur/dilation or artificial filling of rejected guides is applied.
    for key in ('color', 'geometry', 'thin'):
        values = np.array([[float(lookup[x, y][key]) for x in xs] for y in ys])
        horizontal = np.array([np.interp(np.arange(box[0], box[2]), xs, row) for row in values])
        smooth = np.array([np.interp(np.arange(box[1], box[3]), ys, column) for column in horizontal.T]).T
        gray = np.rint(np.clip(smooth, 0, 1) * 255).astype(np.uint8)
        images.append(Image.fromarray(gray).convert('RGB'))
        titles.append(key + ' confidence / bilinear')
    width, height = images[0].size
    result = Image.new('RGB', (width * args.scale * len(images), height * args.scale + 40), (18, 18, 18))
    draw = ImageDraw.Draw(result)
    for index, (image, title) in enumerate(zip(images, titles)):
        x = index * width * args.scale
        result.paste(image.resize((width * args.scale, height * args.scale), Image.Resampling.NEAREST), (x, 40))
        draw.text((x + 6, 12), title, fill='white')
    output = Path(str(args.prefix) + '-review.png')
    result.save(output)
    print(output)


if __name__ == '__main__':
    main()
