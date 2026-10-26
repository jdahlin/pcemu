#!/usr/bin/env python3
"""Bundle native ARM64 VirtIO packages while retaining the known working INTx boot driver.

Windows must still validate catalog signatures when importing the bundle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile
from fetch_virtio_arm64 import ALL_PACKAGES
from inspect_validation_network import decode_inf, pe_architecture

ROOT = Path(__file__).resolve().parents[1]


def build_bundle(drivers, viostor, output):
    files = {}
    for package in ALL_PACKAGES:
        source = viostor if package == 'viostor' else drivers / package
        if not source.is_dir():
            raise ValueError('Missing driver family: ' + package)
        members = [p for p in source.iterdir() if p.is_file() and
                   p.suffix.lower() in ('.sys', '.dll', '.exe', '.inf', '.cat')]
        if not any(p.suffix.lower() == '.inf' for p in members):
            raise ValueError('Missing INF: ' + package)
        for path in sorted(members):
            data = path.read_bytes()
            suffix = path.suffix.lower()
            if suffix in ('.sys', '.dll', '.exe') and pe_architecture(data) != 'ARM64':
                raise ValueError('Non-ARM64 payload: ' + str(path))
            if suffix == '.inf':
                text = decode_inf(data).upper()
                if 'NTARM64' not in text:
                    raise ValueError('Missing ARM64 INF models: ' + str(path))
                if package == 'viostor' and '100.101.104.28500' not in text:
                    raise ValueError('Expected viostor 0.1.285 for our INTx-only boot controller')
            files[package + '/' + path.name] = data
    files['virtio-win_license.txt'] = (drivers / 'virtio-win_license.txt').read_bytes()
    manifest = {'packages': '0.1.302 except viostor 0.1.285 INTx compatibility',
                'files': [{'path': name, 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, data in sorted(files.items())]}
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()):
            archive.writestr(name, data)
        archive.writestr('manifest.json', json.dumps(manifest, indent=2))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--drivers', type=Path, default=ROOT / 'media/virtio-arm64-complete-0.1.302')
    parser.add_argument('--viostor-intx', type=Path, default=ROOT / 'media/viostor-arm64-intx-0.1.285/viostor')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = build_bundle(args.drivers, args.viostor_intx, args.output)
    print(json.dumps({'output': str(args.output), 'files': len(manifest['files']),
                      'sha256': hashlib.sha256(args.output.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
