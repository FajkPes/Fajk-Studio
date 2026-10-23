"""Check random access, reference cache invalidation and SDK output guards."""
import argparse, ctypes as C, hashlib, json, subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from PIL import Image
from compare_sdk_work import Decoder


def check(library, packet, backend, workers, padding, references):
    decoder = Decoder(library, packet, workers)
    records = []
    try:
        lib = decoder.dll
        lib.fajk_set_backend.argtypes = [C.c_void_p, C.c_uint32]
        lib.fajk_set_deblocking.argtypes = [C.c_void_p, C.c_uint32]
        lib.fajk_set_sharpening.argtypes = [C.c_void_p, C.c_uint32]
        w, h = decoder.info.width, decoder.info.height
        stride = w * 4 + padding
        capacity = stride * (h - 1) + w * 4
        for index, selected, deblock, sharpen, expected, expected_pts, expected_duration in references:
            for fn, value in [(lib.fajk_set_backend, selected), (lib.fajk_set_deblocking, deblock), (lib.fajk_set_sharpening, sharpen)]:
                if fn(decoder.handle, value):
                    raise RuntimeError(lib.fajk_last_error(decoder.handle))
            output = (C.c_uint8 * (capacity + 32))()
            C.memset(output, 197, len(output))
            pts, duration = C.c_int64(), C.c_int64()
            if lib.fajk_decode_frame(decoder.handle, index, C.byref(output, 16), capacity, stride, C.byref(pts), C.byref(duration)):
                raise RuntimeError(lib.fajk_last_error(decoder.handle))
            raw, pixels = bytes(output), bytearray()
            if raw[:16] != bytes([197]) * 16 or raw[-16:] != bytes([197]) * 16:
                raise RuntimeError('SDK damaged guards')
            for y in range(h):
                start = 16 + y * stride
                pixels += raw[start:start + w * 4]
                if y < h - 1 and raw[start + w * 4:start + stride] != bytes([197]) * padding:
                    raise RuntimeError('SDK damaged row padding')
            if hashlib.sha256(pixels).hexdigest() != expected:
                raise RuntimeError('SDK random decode/cache differs from CLI')
            if (pts.value, duration.value) != (expected_pts, expected_duration):
                raise RuntimeError('SDK timestamps changed')
            records.append(dict(file=packet.name, index=index, backend=selected, workers=workers, padding=padding, deblock=deblock, sharpen=sharpen))
        # Failure must not poison a subsequent valid read.
        if lib.fajk_decode_frame(decoder.handle, decoder.info.frame_count, C.byref(output, 16), capacity, stride, None, None) != -1:
            raise RuntimeError('SDK accepted invalid frame index')
        index = references[-1][0]
        if lib.fajk_decode_frame(decoder.handle, index, C.byref(output, 16), capacity, stride, None, None):
            raise RuntimeError('SDK failed after rejected read')
    finally:
        decoder.close()
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    raw = args.input.read_bytes()
    count, offset = (int.from_bytes(raw[p:p + 8], 'little') for p in [40, 48])
    timestamp = lambda i, p: int.from_bytes(raw[offset + i * 40 + p:offset + i * 40 + p + 8], 'little')
    jobs = []
    for workers in [1, 2, 10]:
        for backend in [0, 1]:
            references = []
            # Alternating backends and postfilter settings within each handle
            # must invalidate its keyframe reconstruction cache.
            for n, index in enumerate([count - 1, 0, 1, count // 2, 7, count - 1]):
                selected, deblock, sharpen = (backend + n // 2) % 2, (0 if n == 3 else 100), (50 if n == 4 else 0)
                png = args.output / f'w{workers}-b{backend}-n{n}.png'
                subprocess.run([str(args.cli), 'decode', str(args.input), str(index), str(png), '--workers', str(workers), '--deblock', str(deblock), '--sharpen', str(sharpen), '--gpu' if selected else '--cpu'], check=True, capture_output=True)
                with Image.open(png) as image:
                    digest = hashlib.sha256(image.convert('RGBA').tobytes()).hexdigest()
                references.append((index, selected, deblock, sharpen, digest, timestamp(index, 16), timestamp(index, 24)))
            for padding in [0, 28]:
                jobs.append((args.library, args.input, backend, workers, padding, references))
    with ThreadPoolExecutor(max_workers=2) as pool:
        records = [record for group in pool.map(lambda job: check(*job), jobs) for record in group]
    (args.output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    print(f'PASS: {len(records)} SDK random decodes, concurrent handles, settings/backend changes and guarded strides')


if __name__ == '__main__':
    main()
