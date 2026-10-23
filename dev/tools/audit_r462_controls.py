"""Reproduce R462 control and automatic-target issues without changing the release."""
import ctypes as C
import csv
import argparse
import hashlib
import json
import struct
import subprocess
import zlib
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'dev/diagnostics/R462-code-audit-20261018'
OUT.mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk', type=Path, default=ROOT / 'dev/SDK/fajk_decode.dll')
parser.add_argument('--cli', type=Path, default=ROOT / 'release/R462/app/fajk_cli.exe')
parser.add_argument('--expect-fixed', action='store_true')
parser.add_argument('--output', type=Path, default=OUT)
parser.add_argument('--check-allocation-packets', type=Path)
args = parser.parse_args()
OUT=args.output.resolve()
OUT.mkdir(parents=True,exist_ok=True)
DLL = C.CDLL(str(args.sdk.resolve()))
DLL.fajk_open.argtypes = [C.c_char_p, C.c_char_p, C.c_size_t]
DLL.fajk_open.restype = C.c_void_p
DLL.fajk_close.argtypes = [C.c_void_p]
DLL.fajk_set_threads.argtypes = [C.c_void_p, C.c_uint32]
DLL.fajk_set_deblocking.argtypes = [C.c_void_p, C.c_uint32]
DLL.fajk_set_sharpening.argtypes = [C.c_void_p, C.c_uint32]
DLL.fajk_decode_frame.argtypes = [C.c_void_p, C.c_uint64, C.c_void_p, C.c_size_t,
                                 C.c_size_t, C.c_void_p, C.c_void_p]
DLL.fajk_last_error.argtypes = [C.c_void_p]
DLL.fajk_last_error.restype = C.c_char_p


def decode(path, deblock=100, sharpen=0, padding=0):
    data = path.read_bytes()
    w, h = struct.unpack_from('<II', data, 16)
    error = C.create_string_buffer(1024)
    decoder = DLL.fajk_open(str(path).encode('utf-8'), error, len(error))
    if not decoder:
        raise RuntimeError(error.value)
    try:
        assert DLL.fajk_set_threads(decoder, 1) == 0
        assert DLL.fajk_set_deblocking(decoder, deblock) == 0
        assert DLL.fajk_set_sharpening(decoder, sharpen) == 0
        stride=w*4+padding
        pixels = (C.c_uint8 * (stride*h+32))()
        C.memset(pixels,197,len(pixels))
        if DLL.fajk_decode_frame(decoder, 0, C.byref(pixels,16), len(pixels)-32, stride, None, None):
            raise RuntimeError(DLL.fajk_last_error(decoder))
        raw=bytes(pixels)
        assert raw[:16]==bytes([197])*16 and raw[-16:]==bytes([197])*16
        if padding:
            assert all(raw[16+y*stride+w*4:16+(y+1)*stride]==bytes([197])*padding for y in range(h))
        return hashlib.sha256(b''.join(raw[16+y*stride:16+y*stride+w*4] for y in range(h))).hexdigest()
    finally:
        DLL.fajk_close(decoder)


def controls_packet(source, intensity, aa):
    data = bytearray(source.read_bytes())
    index = struct.unpack_from('<Q', data, 48)[0]
    offset, size = struct.unpack_from('<QQ', data, index)
    assert struct.unpack_from('<Q', data, 40)[0] == 1
    assert data[offset + 4] == 130 and size >= 36
    struct.pack_into('<HHHH', data, offset + 20, intensity, 0, aa, 0)
    struct.pack_into('<I', data, index + 32, zlib.crc32(data[offset:offset + size]))
    struct.pack_into('<I', data, 64, zlib.crc32(data[index:]))
    struct.pack_into('<I', data, 68, zlib.crc32(data[:68]))
    result = OUT / f'intensity{intensity}-aa{aa}.fajkv'
    result.write_bytes(data)
    return result


source = ROOT / 'dev/diagnostics/R462-allocation-20261018/timings/30000-flat50.fajkv'
hashes = {f'deblock{d}-sharpen{s}': decode(source, d, s)
          for d, s in ((100, 0), (0, 0), (100, 100))}
aa_hashes = {f'intensity{i}-aa{a}': decode(controls_packet(source, i, a))
             for i, a in ((0, 0), (0, 1), (0, 50), (0, 100), (50, 0), (50, 100))}
small = Image.new('RGB', (64, 64))
small.putdata([(x * 4, y * 4, (x + y) * 2) for y in range(64) for x in range(64)])
sample = OUT / 'gradient64.png'
small.save(sample)
cli = args.cli.resolve()
small_cases = []
for arguments in ([], ['--no-target']):
    output = OUT / ('small-auto.fajk' if not arguments else 'small-no-target.fajk')
    run = subprocess.run([str(cli), 'encode', str(sample), str(output), '--overwrite',
                          *arguments], capture_output=True, text=True)
    small_cases.append({'arguments': arguments, 'exit': run.returncode,
                        'stdout': run.stdout.strip(), 'stderr': run.stderr.strip()})
report = {'sdk_controls': hashes,
          'sdk_controls_all_identical': len(set(hashes.values())) == 1,
          'saved_aa_controls': aa_hashes,
          'aa_ineffective_at_zero_intensity': aa_hashes['intensity0-aa0'] == aa_hashes['intensity0-aa100'],
          'aa_1_50_100_identical_at_zero_intensity': len({aa_hashes[f'intensity0-aa{a}'] for a in (1, 50, 100)}) == 1,
          'aa_effective_at_50_intensity': aa_hashes['intensity50-aa0'] != aa_hashes['intensity50-aa100'],
          'small_image': small_cases}
if args.check_allocation_packets:
    report['sdk_allocation_packets']=[]
    for budget in (30000,80000,150000):
        for value in (0,50,100):
            prefix=args.check_allocation_packets/f'{budget}-flat{value}'
            expected=hashlib.sha256(Image.open(str(prefix)+'.png').convert('RGBA').tobytes()).hexdigest()
            actual=decode(Path(str(prefix)+'.fajk'))
            assert actual==expected,(budget,value,'SDK pixels differ')
            assert decode(Path(str(prefix)+'.fajk'),padding=13)==expected,(budget,value,'SDK stride differs')
            report['sdk_allocation_packets'].append({'budget':budget,'allocation':value,'rgba_and_stride_equal':True})
stages = {}
for budget in (30000, 80000, 150000):
    stage_file = source.parent / f'{budget}-flat50-stages.csv'
    totals = {}
    repeats = set()
    with stage_file.open() as stream:
        for row in csv.DictReader(stream):
            repeats.add(row['repeat'])
            totals[row['stage']] = totals.get(row['stage'], 0.) + float(row['ms'])
    stages[str(budget)] = {name: round(value / len(repeats), 4) for name, value in totals.items()}
report['existing_stage_means_ms'] = stages
(OUT / 'reproductions.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
if args.expect_fixed:
    assert len(set(hashes.values())) == 3, 'Native filter options still disappear.'
    assert len({aa_hashes[f'intensity0-aa{a}'] for a in (1, 50, 100)}) > 1, 'AA control still has no range.'
    assert all(row['exit'] == 0 for row in small_cases), 'Automatic small-image encoding still fails.'
print(json.dumps({key: value for key, value in report.items() if key != 'existing_stage_means_ms'}, indent=2))
for budget, values in stages.items():
    print(budget, {name: values[name] for name in (
        'encode.rate_search_including_pack', 'encode.r6_model_fit', 'encode.source_priority_analysis',
        'decode.unified385.guide', 'decode.unified385.render', 'decode.fast441.cleanup', 'decode.fast441.upscale'
    ) if name in values})
