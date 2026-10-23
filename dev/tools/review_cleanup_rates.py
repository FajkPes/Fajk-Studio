"""Compare default/disabled reconstruction on identical compressed pixels.

Writes only to --output; source packets and historical references stay intact.
All container checksums are updated when editing reconstruction-only settings.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib
import numpy as np
from PIL import Image


def without_cleanup(data):
    data = bytearray(data)
    assert data[:8] == b'FAJKVID\0'
    assert struct.unpack_from('<Q', data, 40)[0] == 1
    index = struct.unpack_from('<Q', data, 48)[0]
    offset, length = struct.unpack_from('<QQ', data, index)
    assert data[offset+4] == 130
    # Only intensity, AA and contrast change. Compressed child stays untouched.
    for field in [20, 24, 26]:
        struct.pack_into('<H', data, offset+field, 0)
    struct.pack_into('<I', data, index+32, zlib.crc32(data[offset:offset+length]))
    struct.pack_into('<I', data, 64, zlib.crc32(data[index:]))
    struct.pack_into('<I', data, 68, zlib.crc32(data[:68]))
    return data


def main():
    dev = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=dev/'build-r4/fajk_cli.exe')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--packets', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=20)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    for sample in ['22920', '22925', '23006']:
        for budget in [30000, 80000, 150000, 300000]:
            name = f'{sample}-{budget}'
            packet = args.packets/(name+'.fajk')
            if not packet.exists():
                packet = args.output/(name+'.fajk')
                if not packet.exists():
                    subprocess.run([str(args.candidate), 'encode', str(dev/'samples/references'/(sample+'-reference.png')),
                                    str(packet), '--r441', '--quality', '100', '--target-bytes', str(budget), '--overwrite'], check=True)
            off = args.output/(name+'-off.fajk')
            off.write_bytes(without_cleanup(packet.read_bytes()))
            for mode, file in [('on', packet), ('off', off)]:
                subprocess.run([str(args.candidate), 'measure-decode', str(file), str(args.output/(name+'-'+mode)),
                                str(args.repeats), '0', 'buffer'], check=True)
            a = np.array(Image.open(args.output/(name+'-on.png')).convert('RGB'), dtype=np.int16)
            b = np.array(Image.open(args.output/(name+'-off.png')).convert('RGB'), dtype=np.int16)
            d = abs(a-b)
            record = dict(sample=sample, budget=budget, bytes=packet.stat().st_size,
                          changed_pixels_pct=float(np.any(d,axis=2).mean()*100), mean_channel_delta=float(d.mean()),
                          max_channel_delta=int(d.max()), p99_channel_delta=float(np.percentile(d,99)))
            records.append(record)
            print(json.dumps(record), flush=True)
    (args.output/'differences.json').write_text(json.dumps(records, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
