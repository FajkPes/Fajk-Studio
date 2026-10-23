"""Decode the same coefficients with isolated cleanup controls.

Writes diagnostic copies only, fixes container checksums, preserves original.
The colored mask shows pixels changed by text protection, never an image edit.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = args.input.read_bytes()
    assert data[:8] == b'FAJKVID\0' and struct.unpack_from('<Q', data, 40)[0] == 1
    index = struct.unpack_from('<Q', data, 48)[0]
    offset, length = struct.unpack_from('<QQ', data, index)
    assert data[offset + 4] == 130
    args.output.mkdir(parents=True, exist_ok=True)
    images = {}
    for label in ('default', 'no-text', 'no-skirt', 'no-contours'):
        changed = bytearray(data)
        if label == 'no-text':
            flags = struct.unpack_from('<H', changed, offset + 28)[0]
            struct.pack_into('<H', changed, offset + 28, flags & ~1)
        if label == 'no-skirt':
            struct.pack_into('<H', changed, offset + 20, 0)
        if label == 'no-contours':
            base = offset + 36
            assert changed[base + 4] == 129
            struct.pack_into('<H', changed, base + 20, 0)
            child = base + 28
            if changed[child + 4] == 77:
                child += 24 + struct.unpack_from('<I', changed, child + 16)[0]
            assert changed[child + 4] == 28 and changed[child + 5] == 130
            child_bytes = struct.unpack_from('<I', changed, child + 24)[0]
            struct.pack_into('<H', changed, child + 28 + child_bytes + 6, 0)
        struct.pack_into('<I', changed, index + 32, zlib.crc32(changed[offset:offset + length]))
        struct.pack_into('<I', changed, 64, zlib.crc32(changed[index:]))
        struct.pack_into('<I', changed, 68, zlib.crc32(changed[:68]))
        packet, png = args.output / (label + '.fajk'), args.output / (label + '.png')
        packet.write_bytes(changed)
        subprocess.run([str(args.exe.resolve()), 'decode', str(packet), '0', str(png)], check=True)
        images[label] = np.asarray(Image.open(png).convert('RGB')).astype(np.int16)
    records = {}
    for label, image in images.items():
        difference = np.abs(image - images['default'])
        mask = np.max(difference, axis=2)
        records[label] = dict(changed_pixels=int(np.count_nonzero(mask)),
                              mean_rgb_difference=float(difference.mean()), max_rgb_difference=int(difference.max()))
        color = np.zeros_like(image, dtype=np.uint8)
        color[:, :, 0] = np.minimum(255, mask * 16).astype(np.uint8)
        color[:, :, 1] = np.where(mask > 0, 150, 0)
        Image.fromarray(color).save(args.output / (label + '-mask.png'))
    (args.output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    print(json.dumps(records, indent=2))


if __name__ == '__main__':
    main()
