"""Check the current R441 CLI against frozen R441 packets and RGBA hashes.

The manifest comes from the approved pre-cleanup R441 implementation. Never
regenerate expectations with the candidate being tested.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from PIL import Image


def main():
    dev = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=dev / 'build-r4/fajk_cli.exe')
    parser.add_argument('--output', type=Path, default=dev / 'diagnostics/legacy-regression-check')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixtures = dev / 'samples/regression'
    manifest = json.loads((fixtures / 'manifest.json').read_text('utf-8'))
    records = []
    for case in manifest:
        name = f"{case['sample']}-{case['budget']}"
        packet, png = args.output / (name + '.fajk'), args.output / (name + '.png')
        source = dev / 'samples/references' / (case['sample'] + '-reference.png')
        for command in [
            ['encode', source, packet, '--r441', '--quality', '100', '--target-bytes', str(case['budget']), '--workers', '1', '--overwrite'],
            ['decode', fixtures / (name + '.fajk'), '0', png, '--sharpen', '0'],
        ]:
            result = subprocess.run([str(args.candidate.resolve()), *map(str, command)], capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
        with Image.open(png) as image:
            rgba_hash = hashlib.sha256(image.convert('RGBA').tobytes()).hexdigest()
        packet_hash = hashlib.sha256(packet.read_bytes()).hexdigest()
        records.append(dict(name=name, identical_packet=packet_hash == case['packet_sha256'],
                            identical_rgba=rgba_hash == case['rgba_sha256']))
        print(name, records[-1], flush=True)
    (args.output / 'regression.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    if not all(r['identical_packet'] and r['identical_rgba'] for r in records):
        raise SystemExit('Current R441 regression')


if __name__ == '__main__':
    main()
