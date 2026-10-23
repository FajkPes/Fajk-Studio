"""Compare decoded final sharpening and high-rate contours against frozen R490 .2."""
import argparse, ctypes as C, hashlib, json, statistics, subprocess, time
from pathlib import Path
import numpy as np
from PIL import Image
from compare_sdk_work import Decoder


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['baseline_cli', 'baseline_dll', 'cli', 'dll', 'output']:
        p.add_argument('--' + name.replace('_', '-'), type=Path, required=True)
    p.add_argument('--sources', type=Path, nargs='+', required=True)
    p.add_argument('--metric', type=Path, required=True)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=True)
    cases, saved = [], []
    for source_number, source in enumerate(a.sources):
        for budget in [30000, 80000, 150000]:
            for intensity in ([0, 50] if not source_number else [50]):
                name = f'{source.stem}-{budget}-contour{intensity}'
                packet = a.output / (name + '.fajk')
                flags = ['--fast', '--no-aq', '--gpu', '--422', '--protect-text', '0', '--antialias', '0', '--r460-intensity', str(intensity), '--workers', '10', '--target-bytes', str(budget), '--overwrite']
                subprocess.run([str(a.baseline_cli), 'encode', str(source), str(packet), *flags], check=True, capture_output=True)
                current = a.output / (name + '-current.fajk')
                subprocess.run([str(a.cli), 'encode', str(source), str(current), *flags], check=True, capture_output=True)
                assert packet.read_bytes() == current.read_bytes(), 'Display filters altered encoded data'
                for backend in [0, 1]:
                    decoders = [Decoder(dll, packet, 10) for dll in [a.baseline_dll, a.dll]]
                    record = dict(source=str(source), budget=budget, contours=intensity, backend=backend, times={}, effects={})
                    try:
                        for decoder in decoders:
                            decoder.dll.fajk_set_backend.argtypes = [C.c_void_p, C.c_uint32]
                            decoder.dll.fajk_set_sharpening.argtypes = [C.c_void_p, C.c_uint32]
                            assert not decoder.dll.fajk_set_backend(decoder.handle, backend)
                        images = {}
                        for strength in [0, 50, 100]:
                            samples = [[], []]
                            for decoder in decoders:
                                assert not decoder.dll.fajk_set_sharpening(decoder.handle, strength)
                                for _ in range(2): decoder.decode()
                            for round_number in range(8):
                                for i in ([0, 1] if not round_number % 2 else [1, 0]):
                                    start = time.perf_counter(); decoders[i].decode(); samples[i].append((time.perf_counter() - start) * 1000)
                            for i, decoder in enumerate(decoders):
                                key = f'{"old" if not i else "new"}-{strength}'
                                record['times'][key] = statistics.mean(samples[i])
                                record.setdefault('timing_samples', {})[key] = samples[i]
                                # Guard validation and PNG export are untimed.
                                decoder.digest(); raw = bytes(decoder.buffer)[16:-16]
                                image = np.frombuffer(raw, dtype=np.uint8).reshape(decoder.info.height, decoder.info.width, 4).copy()
                                images[key] = image
                                if intensity == 50:
                                    png = a.output / f'{name}-b{backend}-{key}.png'
                                    Image.fromarray(image).save(png)
                            for label in ['old', 'new']:
                                if strength:
                                    diff = np.abs(images[f'{label}-{strength}'].astype(np.int16) - images[f'{label}-0'].astype(np.int16))
                                    record['effects'][f'{label}-{strength}'] = dict(mean=float(diff[..., :3].mean()), changed_fraction=float((diff[..., :3].max(axis=2) != 0).mean()), maximum=int(diff.max()))
                        if not intensity:
                            assert np.array_equal(images['old-0'], images['new-0']), 'Unrelated decode changed with controls off'
                        if intensity == 50 and budget == 30000:
                            assert np.array_equal(images['old-0'], images['new-0']), 'Approved 30 kB contour endpoint changed'
                        assert record['effects']['new-100']['changed_fraction'] > 0.001, 'Final sharpening is ineffective'
                        assert record['effects']['new-100']['mean'] >= record['effects']['new-50']['mean'], 'Sharpening slider is not monotonic'
                        cases.append(record)
                        (a.output / 'progress.json').write_text(json.dumps(cases, indent=2), encoding='utf-8')
                        print(name, 'gpu' if backend else 'cpu', record['times'], flush=True)
                    finally:
                        for decoder in decoders: decoder.close()
    # Perceptual metrics run only after all performance measurements.
    for case in cases:
        if case['contours'] != 50: continue
        name = f'{Path(case["source"]).stem}-{case["budget"]}-contour50-b{case["backend"]}'
        paths = [a.output / f'{name}-{label}-{strength}.png' for label, strength in [('old', 0), ('new', 0), ('new', 50), ('new', 100)]]
        scores = list(map(float, subprocess.check_output([str(a.metric), case['source'], *map(str, paths)], text=True).splitlines()))
        case['ss2'] = dict(zip(['old0', 'new0', 'new50', 'new100'], scores))
        print(name, case['ss2'], flush=True)
    (a.output / 'results.json').write_text(json.dumps(dict(complete=True, cases=cases), indent=2), encoding='utf-8')
    print('PASS final filters, active controls, exact encoded packets and control-off compatibility')


if __name__ == '__main__': main()
