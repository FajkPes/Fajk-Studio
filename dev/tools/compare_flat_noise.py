"""Measure unwanted fine luma/chroma error in source-flat snapshot regions.

No image is edited. The source chooses one common flat mask for both outputs.
High-pass error measures speckle; chroma RMSE also catches broad color errors.
These snapshot metrics do not establish whole-video temporal stability.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image


def read(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)


def measure(source, output, mask):
    error = output-source
    channels = np.stack(((error[..., 0]+2*error[..., 1]+error[..., 2])*.25,
                         (error[..., 0]-error[..., 2])*.5,
                         (2*error[..., 1]-error[..., 0]-error[..., 2])*.25), axis=-1)
    high = channels[1:-1, 1:-1] - .25*(channels[:-2, 1:-1]+channels[2:, 1:-1]+
                                                     channels[1:-1, :-2]+channels[1:-1, 2:])
    return dict(luma_speckle_rms=float(np.sqrt(np.mean(high[..., 0][mask]**2))),
                chroma_speckle_rms=float(np.sqrt(np.mean(high[..., 1:][mask]**2))),
                chroma_error_rms=float(np.sqrt(np.mean(channels[1:-1, 1:-1, 1:][mask]**2))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('before', 'after', 'output'):
        parser.add_argument('--'+flag, type=Path, required=True)
    parser.add_argument('--before-suffix', choices=('max', 'intra'), default='max',
                        help='Probe image suffix; intra denotes its chosen baseline, not necessarily an intra encode')
    parser.add_argument('--frames', type=int, nargs='+', default=[1, 12, 23, 47, 119, 239])
    args = parser.parse_args()
    rows = []
    for frame in args.frames:
        source = read(args.after / f'{frame}-source.png')
        center = source[1:-1, 1:-1]
        difference = np.maximum.reduce([np.max(np.abs(center-neighbor), axis=-1) for neighbor in
                         (source[:-2, 1:-1], source[2:, 1:-1], source[1:-1, :-2], source[1:-1, 2:])])
        mask = difference <= 4
        assert np.count_nonzero(mask) > 0
        before = measure(source, read(args.before / f'{frame}-{args.before_suffix}.png'), mask)
        after = measure(source, read(args.after / f'{frame}-max.png'), mask)
        rows.append(dict(frame=frame, samples=int(np.count_nonzero(mask)), before=before, after=after,
                         change_percent={key: 100*(after[key]/before[key]-1) for key in before}))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(dict(complete=True, source_flat_rgb_limit=4, records=rows), indent=2))
    for row in rows:
        print(row['frame'], row['change_percent'])


if __name__ == '__main__':
    main()
