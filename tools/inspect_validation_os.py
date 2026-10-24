#!/usr/bin/env python3
"""Inspect the ARM64 WIM and extract only Windows Boot Manager/BCD template.

Requires wimlib-imagex (offline tooling only, never linked into the emulator).
The optional loader probe is diagnostic media, not a complete Windows boot disk.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
from make_uefi_test_disk import create

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wim', type=Path, default=ROOT / 'media/validation-os-arm64/ValidationOS.wim')
    parser.add_argument('--wimlib', default=shutil.which('wimlib-imagex') or
                        str(ROOT / 'build/media-tools/wimlib/wimlib-imagex'))
    parser.add_argument('--output', type=Path, default=ROOT / 'build/validation-boot')
    parser.add_argument('--make-loader-probe', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    command = [args.wimlib, 'dir', str(args.wim), '2']
    listing = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    files = [line for line in listing.splitlines() if line.startswith('/')]
    report = {'wim': str(args.wim), 'image': 2, 'drivers': {}, 'extracted': [],
              'windowsBootSupported': False}
    for driver in ('viostor.sys', 'vioscsi.sys', 'netkvm.sys', 'stornvme.sys', 'acpi.sys'):
        report['drivers'][driver] = [path for path in files if path.rsplit('/', 1)[-1].lower() == driver]
    for source, target in (
        ('/Windows/Boot/EFI/bootmgfw.efi', 'bootmgfw.efi'),
        ('/Windows/System32/config/BCD-Template', 'BCD-Template'),
    ):
        result = subprocess.run([args.wimlib, 'extract', str(args.wim), '2', source, '--to-stdout'],
                                check=True, capture_output=True)
        data = result.stdout
        if target.endswith('.efi'):
            if data[:2] != b'MZ':
                raise RuntimeError('Expected PE Windows loader')
            pe = struct.unpack_from('<I', data, 60)[0]
            if data[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', data, pe + 4)[0] != 0xaa64:
                raise RuntimeError('Windows loader is not ARM64')
        path = args.output / target
        if path.exists() and path.read_bytes() != data:
            raise RuntimeError(f'Refusing to overwrite different payload: {path}')
        path.write_bytes(data)
        report['extracted'].append({'path': target, 'wimPath': source, 'bytes': len(data),
                                    'sha256': hashlib.sha256(data).hexdigest()})
    if args.make_loader_probe:
        create(args.output / 'loader-probe.raw', args.output / 'bootmgfw.efi')
        report['loaderProbe'] = 'loader-probe.raw'
    (args.output / 'inventory.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
