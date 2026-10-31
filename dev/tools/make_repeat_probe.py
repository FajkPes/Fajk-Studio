"""Insert one exact hold after each frame without changing compressed pictures.

Diagnostic fixture for first-hold cache reuse. Split each source duration in
two; remap references, preserve the original timeline, and recompute all CRCs.
No source image is decoded or re-encoded. Output must be a new file.
"""
import argparse
import struct
import zlib
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=0)
    args = parser.parse_args()
    data = args.source.read_bytes()
    assert data[:8] == b'FAJKVID\0' and len(data) >= 128
    assert struct.unpack_from('<II', data, 8) == (3, 128), 'Requires a version-3 sequence'
    assert zlib.crc32(data[:68]) == struct.unpack_from('<I', data, 68)[0]
    count, offset = struct.unpack_from('<QQ', data, 40)
    assert 0 < count <= 10_000_000 and offset >= 128 and offset + count * 40 == len(data)
    index = data[offset:]
    assert zlib.crc32(index) == struct.unpack_from('<I', data, 64)[0]
    limit = min(count, args.frames) if args.frames else count
    assert 0 < limit <= 5_000_000
    output, entries = bytearray(128), bytearray()
    end = 0
    for n in range(limit):
        start, size, pts, duration, checksum, reference = struct.unpack_from('<QQqqII', index, n * 40)
        assert 128 <= start <= offset and 5 <= size <= offset - start and 0 <= reference <= n
        payload = data[start:start + size]
        assert zlib.crc32(payload) == checksum and duration >= 2
        assert bool(reference) == (payload[4] in (136, 137))
        first = duration // 2
        entries.extend(struct.pack('<QQqqII', len(output), size, pts, first, checksum,
                                   (reference - 1) * 2 + 1 if reference else 0))
        output.extend(payload)
        repeat = struct.pack('<IBBH', 0x334B4A46, 137 if payload[4] == 137 else 136, 2, 0)
        entries.extend(struct.pack('<QQqqII', len(output), len(repeat), pts + first,
                                   duration - first, zlib.crc32(repeat), n * 2 + 1))
        output.extend(repeat)
        end = pts + duration
    header = bytearray(data[:128])
    struct.pack_into('<QQQ', header, 40, limit * 2, len(output), end)
    struct.pack_into('<I', header, 64, zlib.crc32(entries))
    struct.pack_into('<I', header, 68, zlib.crc32(header[:68]))
    output[:128] = header
    output.extend(entries)
    with args.output.open('xb') as stream:
        stream.write(output)
    print(f'PASS: {limit} original packets + {limit} exact holds, original timeline, {len(output)} bytes')


if __name__ == '__main__':
    main()
