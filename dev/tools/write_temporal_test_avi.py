"""Create a tiny, exact RGB AVI fixture without third-party video encoders.

Includes holds beyond the keyframe interval, local changes, return to the first
picture and a scene cut. Windows Media Foundation reads the uncompressed DIB.
"""
import argparse, struct
from pathlib import Path
from PIL import Image, ImageDraw


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + b'\0' * (len(data) & 1)


def create(photo, destination):
    width, height, count, fps = 640, 384, 32, 30
    base = Image.open(photo).convert('RGB').resize((width, height))
    frames = []
    for n in range(count):
        image = base.copy()
        if 10 <= n < 22:
            x = 90 + (n - 10) * 4
            ImageDraw.Draw(image).rectangle((x, 100, x + 31, 123), fill=(235, 40, 80))
        elif n >= 26:
            image = Image.eval(image, lambda value: 255 - value)
        frames.append(image.transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes('raw', 'BGR'))
    size = width * height * 3
    avih = struct.pack('<14I', 1000000 // fps, size * fps, 0, 0x10, count, 0, 1, size, width, height, 0, 0, 0, 0)
    strh = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'vids', b'DIB ', 0, 0, 0, 0, 1, fps, 0, count, size, 0xffffffff, 0, 0, 0, width, height)
    strf = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, size, 0, 0, 0, 0)
    header = chunk(b'LIST', b'hdrl' + chunk(b'avih', avih) + chunk(b'LIST', b'strl' + chunk(b'strh', strh) + chunk(b'strf', strf)))
    movie, index, offset = bytearray(b'movi'), bytearray(), 4
    for frame in frames:
        data = chunk(b'00db', frame)
        index += struct.pack('<4sIII', b'00db', 0x10, offset, len(frame))
        movie += data
        offset += len(data)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(chunk(b'RIFF', b'AVI ' + header + chunk(b'LIST', movie) + chunk(b'idx1', index)))
    print(f'{destination}: {count} exact RGB frames, {width}x{height}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('photo', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    create(args.photo, args.destination)
