"""Exact RGB fixture: a dependent image held for 12 s, then changed again."""
import argparse
import struct
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw
from write_temporal_test_avi import chunk


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('photo', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    width, height, fps, count = 320, 192, 30, 420
    base = Image.open(a.photo).convert('RGB').resize((width, height))
    first = ImageChops.add(base, Image.new('RGB', base.size, (2, 2, 2)))
    second = first.copy()
    ImageDraw.Draw(second).rectangle((110, 80, 125, 95), fill=(220, 40, 70))
    pictures = [im.transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes('raw', 'BGR') for im in [base, first, second]]
    size = width * height * 3
    avih = struct.pack('<14I', 1000000 // fps, size * fps, 0, 0x10, count, 0, 1, size, width, height, 0, 0, 0, 0)
    strh = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'vids', b'DIB ', 0, 0, 0, 0, 1, fps, 0, count, size, 0xffffffff, 0, 0, 0, width, height)
    strf = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, size, 0, 0, 0, 0)
    header = chunk(b'LIST', b'hdrl' + chunk(b'avih', avih) + chunk(b'LIST', b'strl' + chunk(b'strh', strh) + chunk(b'strf', strf)))
    movie, index, offset = bytearray(b'movi'), bytearray(), 4
    for n in range(count):
        picture = pictures[0 if n == 0 else 1 if n < 360 else 2]
        block = chunk(b'00db', picture)
        index += struct.pack('<4sIII', b'00db', 0x10, offset, len(picture))
        movie += block
        offset += len(block)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(chunk(b'RIFF', b'AVI ' + header + chunk(b'LIST', movie) + chunk(b'idx1', index)))
    print('420 exact frames, 14 s, changes at frame 1 and 360')


if __name__ == '__main__':
    main()
