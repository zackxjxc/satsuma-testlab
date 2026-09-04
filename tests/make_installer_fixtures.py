"""AI-authored, Windows-only fixtures for isolated Guest installer acceptance.

These are synthetic version-resource fixtures, NOT historical/future releases.
Never publish them or use their hashes as a production Agent expectation.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    source = options.agent.read_bytes()
    if source[:2] != b'MZ' or 'Satsuma TestLab Guest Agent'.encode('utf-16-le') not in source:
        raise ValueError('Expected a Satsuma Guest executable')
    imagehlp = ctypes.WinDLL('imagehlp', use_last_error=True)
    checksum = imagehlp.MapFileAndCheckSumW
    checksum.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
    checksum.restype = ctypes.c_uint32
    options.output.mkdir(parents=True, exist_ok=False)
    manifest = {'synthetic_versions': True, 'files': {}}
    for name, version in [('SatsumaVM.exe', None), ('same.exe', 'same'), ('older.exe', '0.3.3'), ('newer.exe', '0.3.5')]:
        data = source
        if version and version != 'same':
            old = '0.3.4\0'.encode('utf-16-le')
            if data.count(old) != 2:
                raise ValueError('Expected exactly two 0.3.4 version strings')
            data = data.replace(old, (version + '\0').encode('utf-16-le'))
            fixed = struct.pack('<6I', 0xFEEF04BD, 0x10000, 3, 4 << 16, 3, 4 << 16)
            patch = int(version.rsplit('.', 1)[1])
            replacement = struct.pack('<6I', 0xFEEF04BD, 0x10000, 3, patch << 16, 3, patch << 16)
            if data.count(fixed) != 1:
                raise ValueError('Expected one fixed version resource')
            data = data.replace(fixed, replacement)
        if version == 'same':
            data += b'\nSatsuma isolated same-version fixture\n'
        target = options.output / name
        target.write_bytes(data)
        if version:
            original, calculated = ctypes.c_uint32(), ctypes.c_uint32()
            if checksum(str(target.resolve()), ctypes.byref(original), ctypes.byref(calculated)):
                raise OSError('Cannot calculate fixture PE checksum')
            offset = struct.unpack_from('<I', data, 0x3C)[0] + 24 + 64
            edited = bytearray(data)
            struct.pack_into('<I', edited, offset, calculated.value)
            target.write_bytes(edited)
        manifest['files'][name] = hashlib.sha256(target.read_bytes()).hexdigest()
    target = options.output / 'probe.exe'
    target.write_bytes(options.probe.read_bytes())
    manifest['files']['probe.exe'] = hashlib.sha256(target.read_bytes()).hexdigest()
    (options.output / 'fixtures.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest))


if __name__ == '__main__':
    main()
