#!/usr/bin/env python3
"""Extract the pinned upstream Windows 11 ARM64 NetKVM package using HTTP ranges.

Requires the same build/media-tools pycdlib dependency as fetch_validation_os.py.
This stages driver media; it does not install or load a Windows driver.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
from fetch_validation_os import RemoteISO, pycdlib
from inspect_validation_network import decode_inf, pe_architecture

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ('https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/'
          'archive-virtio/virtio-win-0.1.302-1/virtio-win-0.1.302.iso')
ISO_BYTES = 877373440
NAMES = ('netkvm.inf', 'netkvm.cat', 'netkvm.sys', 'netkvmco.exe', 'netkvmp.exe', 'Readme.md')


def validate_package(files):
    for name in ('netkvm.sys', 'netkvmco.exe', 'netkvmp.exe'):
        if pe_architecture(files[name]) != 'ARM64':
            raise ValueError(f'{name} is not ARM64')
    text = decode_inf(files['netkvm.inf'])
    if not re.search(r'(?im)^\s*%[^%]+%\s*=\s*[^\r\n]*NTARM64', text):
        raise ValueError('INF does not declare ARM64 models')
    if not re.search(r'(?i)PCI\\VEN_1AF4&DEV_1041', text):
        raise ValueError('INF does not match modern virtio-net PCI')
    if not re.search(r'(?im)^\s*CatalogFile(?:\.NTARM64)?\s*=\s*netkvm\.cat\s*$', text):
        raise ValueError('Unexpected catalog file')
    # Presence/PE checks are not Authenticode verification; Windows verifies trust.
    if not files['netkvm.cat'] or files['netkvm.cat'][0] != 0x30:
        raise ValueError('Expected DER catalog')
    match = re.search(r'(?im)^\s*DriverVer\s*=\s*([^\r\n]+)', text)
    return {'architecture': 'ARM64', 'hardwareId': 'PCI\\VEN_1AF4&DEV_1041',
            'driverVer': match.group(1).strip() if match else None,
            'windowsSignatureValidated': False, 'installed': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, default=ROOT / 'media/netkvm-arm64-0.1.302')
    args = parser.parse_args()
    args.destination.mkdir(parents=True, exist_ok=False)
    remote = RemoteISO(32 * 1024 * 1024, source=SOURCE)
    if remote.size != ISO_BYTES:
        raise RuntimeError('Pinned ISO size changed')
    iso = pycdlib.PyCdlib()
    iso.open_fp(remote)
    files = {}
    for name in (*NAMES, 'virtio-win_license.txt'):
        path = '/' + name if name == 'virtio-win_license.txt' else '/NetKVM/w11/ARM64/' + name
        if iso.get_record(iso_path=path).get_data_length() > 8 * 1024 * 1024:
            raise RuntimeError('Unexpectedly large driver file')
        out = io.BytesIO()
        iso.get_file_from_iso_fp(out, iso_path=path, blocksize=remote.block_size)
        files[name] = out.getvalue()
    report = {'source': SOURCE, 'sourceISOBytes': remote.size, 'sourceETag': remote.etag,
              'downloadedBytes': remote.fetched, **validate_package(files),
              'files': [{'name': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                        for name, data in files.items()]}
    for name, data in files.items():
        (args.destination / name).write_bytes(data)
    (args.destination / 'download.json').write_text(json.dumps(report, indent=2) + '\n')
    iso.close()
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
