"""Verify native release versions/hashes and create a checked portable archive.

Personal settings and smoke-test output are excluded. Archive creation is
atomic; an existing distribution is never silently replaced.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import uuid
import zipfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def version(path):
    api = C.WinDLL('version', use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [C.c_wchar_p, C.POINTER(C.c_uint32)]
    api.GetFileVersionInfoW.argtypes = [C.c_wchar_p, C.c_uint32, C.c_uint32, C.c_void_p]
    api.VerQueryValueW.argtypes = [C.c_void_p, C.c_wchar_p, C.POINTER(C.c_void_p), C.POINTER(C.c_uint32)]
    unused = C.c_uint32()
    size = api.GetFileVersionInfoSizeW(str(path), C.byref(unused))
    assert size, f'Missing version resource: {path}'
    data = C.create_string_buffer(size)
    assert api.GetFileVersionInfoW(str(path), 0, size, data)
    pointer, length = C.c_void_p(), C.c_uint32()
    assert api.VerQueryValueW(data, '\\', C.byref(pointer), C.byref(length)) and length.value >= 52
    fixed = C.cast(pointer, C.POINTER(C.c_uint32 * 13)).contents
    assert fixed[0] == 0xFEEF04BD
    return '.'.join(map(str, [fixed[2] >> 16, fixed[2] & 65535, fixed[3] >> 16, fixed[3] & 65535]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ['package', 'build', 'archive']:
        parser.add_argument('--' + flag, type=Path, required=True)
    parser.add_argument('--validation', type=Path)
    args = parser.parse_args()
    package, build, archive = (x.resolve() for x in [args.package, args.build, args.archive])
    assert not archive.exists(), 'Archive already exists; choose a new output filename.'
    names = {'app/FajkStudio.exe': 'FajkStudio.exe', 'app/fajk_cli.exe': 'fajk_cli.exe',
             'app/SDK/fajk_decode.dll': 'fajk_decode.dll', 'app/SDK/fajk_decode.lib': 'fajk_decode.lib'}
    expected_version = version(build / 'FajkStudio.exe')
    for target, source in names.items():
        assert digest(package / target) == digest(build / source), f'Stale packaged binary: {target}'
        if source.endswith(('.exe', '.dll')):
            assert version(package / target) == expected_version, f'Mixed binary versions: {target}'
    if args.validation:
        validation = json.loads(args.validation.read_text())
        assert validation['complete'] and all('sdk' in r for r in validation['records'] if r['tolerance'] is not None)
    files = sorted(p for p in (package / 'app').rglob('*') if p.is_file())
    files += [package / name for name in ['Spustit Fajk Studio.cmd', 'Fajk CLI.cmd', 'README.txt']]
    # A fresh publisher stages only runtime/source assets, but keep this check
    # explicit so diagnostic output cannot accidentally enter a later archive.
    assert not any(p.name in ['settings.ini', 'ui-smoke.txt', 'startup-trace.txt'] for p in files)
    hashes = {p.relative_to(package).as_posix(): digest(p) for p in files}
    manifest = dict(version=expected_version, files=hashes)
    if args.validation:
        manifest['verified_export_results_sha256'] = digest(args.validation)
    (package / 'BUILD.json').write_text(json.dumps(manifest, indent=2))
    files.append(package / 'BUILD.json')
    archive.parent.mkdir(parents=True, exist_ok=True)
    temporary = archive.with_name(archive.name + '.' + uuid.uuid4().hex + '.partial')
    try:
        with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as output:
            for path in files:
                output.write(path, path.relative_to(package).as_posix())
        with zipfile.ZipFile(temporary) as output:
            assert output.testzip() is None
            for path, expected in hashes.items():
                assert hashlib.sha256(output.read(path)).hexdigest() == expected, f'Archive changed {path}'
        os.replace(temporary, archive)
    finally:
        temporary.unlink(missing_ok=True)
    print(f'PASS {expected_version}, {len(files)} verified files, {archive.stat().st_size} B, SHA256 {digest(archive)}')


if __name__ == '__main__':
    main()
