#!/usr/bin/env python3
"""Extract pinned ARM64 SPICE user agents from UTM's verified guest-tools release.

Requires 7zz. No drivers or other installer components are installed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
URL = 'https://github.com/utmapp/qemu/releases/download/v10.0.12-utm/utm-guest-tools-0.1.274.exe'
SHA256 = 'd3691378dffcde986c2e09e391f449477a096818113b3e5e46c40f4a6c89f23c'
FILES = {'vdagent.exe': 'cc5167af34bf5d6dc594e85d0c309af64c1e22b9f1297a5ef69ca99a403c479a',
         'vdservice.exe': '4862a0ecd865c3f9ede015df4639ab97a4a166a0580e0644ab2f42052b629060'}


def verify(path, digest):
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError(f'Hash mismatch: {path.name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installer', type=Path, default=ROOT / 'media/utm-guest-tools-0.1.274.exe')
    parser.add_argument('--destination', type=Path, default=ROOT / 'media/spice-agent-arm64-utm-0.1.274')
    args = parser.parse_args()
    if args.destination.exists():
        for name, digest in FILES.items(): verify(args.destination / 'ARM64' / name, digest)
        print(f'Verified ARM64 SPICE agents: {args.destination}')
        return
    args.installer.parent.mkdir(parents=True, exist_ok=True)
    if not args.installer.exists():
        partial = args.installer.with_suffix('.partial')
        try:
            with urlopen(URL, timeout=60) as response, partial.open('xb') as output:
                shutil.copyfileobj(response, output)
            verify(partial, SHA256)
            partial.rename(args.installer)
        finally:
            partial.unlink(missing_ok=True)
    verify(args.installer, SHA256)
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='spice-extract-', dir=args.destination.parent) as temp:
        subprocess.run(['7zz', 'x', '-y', f'-o{temp}', str(args.installer),
                        *[f'ARM64/{name}' for name in FILES]], check=True, stdout=subprocess.DEVNULL)
        for name, digest in FILES.items():
            path = Path(temp) / 'ARM64' / name
            verify(path, digest)
            data = path.read_bytes()
            pe = struct.unpack_from('<I', data, 0x3c)[0]
            if data[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', data, pe + 4)[0] != 0xaa64:
                raise ValueError('Expected native ARM64 PE executable')
        (Path(temp) / 'manifest.json').write_text(json.dumps({'source': URL, 'installerSHA256': SHA256, 'files': FILES}, indent=2) + '\n')
        shutil.copytree(temp, args.destination)
    print(f'Extracted verified ARM64 SPICE agents: {args.destination}')


if __name__ == '__main__':
    main()
