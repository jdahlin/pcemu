#!/usr/bin/env python3
"""Stage pinned Windows 11 ARM64 VirtIO packages for offline installation.

Signature trust is checked by Windows when loading/installing each driver.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
from fetch_netkvm_arm64 import SOURCE, ISO_BYTES, RemoteISO, pycdlib
from inspect_validation_network import decode_inf, pe_architecture

ROOT = Path(__file__).resolve().parents[1]
INTX_SOURCE = ('https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/'
               'archive-virtio/virtio-win-0.1.285-1/virtio-win-0.1.285.iso')
INTX_ISO_BYTES = 789645312
PACKAGES = ('viostor', 'vioscsi', 'viorng', 'vioserial', 'vioinput', 'Balloon')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, default=ROOT / 'media/virtio-arm64-0.1.302')
    parser.add_argument('--viostor-intx', action='store_true', help='Stage the pre-regression viostor 0.1.285 package for INTx-only devices')
    args = parser.parse_args()
    if args.destination.exists():
        parser.error('Refusing an existing destination')
    source_url = INTX_SOURCE if args.viostor_intx else SOURCE
    expected_size = INTX_ISO_BYTES if args.viostor_intx else ISO_BYTES
    remote = RemoteISO(64 * 1024 * 1024, source=source_url)
    if remote.size != expected_size:
        raise ValueError('Pinned source ISO size changed')
    iso = pycdlib.PyCdlib(); iso.open_fp(remote)
    files = {}
    try:
        for package in (('viostor',) if args.viostor_intx else PACKAGES):
            directory = '/' + package + '/w11/ARM64'
            names = [record.file_identifier().decode() for record in iso.list_children(iso_path=directory)]
            for name in names:
                if Path(name).suffix.lower() not in ('.sys', '.dll', '.exe', '.inf', '.cat'):
                    continue
                source = directory + '/' + name
                size = iso.get_record(iso_path=source).get_data_length()
                if not 0 < size <= 8 * 1024 * 1024:
                    raise ValueError('Unexpected driver size: ' + source)
                stream = io.BytesIO()
                iso.get_file_from_iso_fp(stream, iso_path=source, blocksize=remote.block_size)
                data = stream.getvalue()
                suffix = Path(name).suffix.lower()
                if suffix in ('.sys', '.dll', '.exe') and pe_architecture(data) != 'ARM64':
                    raise ValueError('Not ARM64: ' + source)
                if suffix == '.inf' and 'NTARM64' not in decode_inf(data).upper():
                    raise ValueError('No ARM64 INF models: ' + source)
                if suffix == '.cat' and data[0] != 0x30:
                    raise ValueError('Invalid catalog: ' + source)
                files[package + '/' + name] = data
        stream = io.BytesIO()
        iso.get_file_from_iso_fp(stream, iso_path='/virtio-win_license.txt', blocksize=remote.block_size)
        files['virtio-win_license.txt'] = stream.getvalue()
        report = {'source': source_url, 'sourceISOBytes': remote.size, 'sourceETag': remote.etag,
                  'downloadedBytes': remote.fetched, 'architecture': 'ARM64',
                  'windowsSignatureValidated': False, 'installed': False,
                  'files': [{'name': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                            for name, data in files.items()]}
    finally:
        iso.close()
    args.destination.mkdir(parents=True)
    for name, data in files.items():
        path = args.destination / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
    (args.destination / 'download.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
