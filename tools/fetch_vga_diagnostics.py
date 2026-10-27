#!/usr/bin/env python3
"""Fetch checksum-pinned external VGA diagnostics into ignored build storage."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VGATEST_URL = 'https://github.com/barotto/VGATEST/releases/download/v1.5/VGATEST-1.5-DOS.zip'
VGATEST_SHA = 'fd5a229310a05e18008864642b510e6033999df17111f4ce66fa408a50ac9681'
XVESA_REVISION = '96ff78f92f7b6832046f42486641bf9dafbc29b6'
XVESA_URL = f'https://raw.githubusercontent.com/Marco-Pistella/X-VESA/{XVESA_REVISION}/Release/X-VESA.COM'
XVESA_SHA = '15040fafa9749139e97ca51f719ebffc12d33a6e2fa4d2739854573320bac366'


def fetch(url, digest, destination):
    """Download with TLS verification and reject altered content before use."""
    if destination.exists() and hashlib.sha256(destination.read_bytes()).hexdigest() == digest:
        return destination.read_bytes()
    data = subprocess.check_output(['curl', '--fail', '--location', '--silent', '--show-error',
                                    '--max-time', '60', url])
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f'Diagnostic checksum mismatch: {url}')
    destination.write_bytes(data)
    return data


def prepare(folder):
    """Extract explicit ZIP members only; preserve licenses beside the binaries."""
    folder.mkdir(parents=True, exist_ok=True)
    data = fetch(VGATEST_URL, VGATEST_SHA, folder / 'vgatest-1.5.zip')
    files = {}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for name in ('VGATEST0.EXE', 'VGATEST.EXE', 'dos4gw.exe', 'COPYING', 'README', 'VERSION'):
            content = archive.read(name)
            (folder / name.upper()).write_bytes(content)
            files[name.upper()] = hashlib.sha256(content).hexdigest()
    data = fetch(XVESA_URL, XVESA_SHA, folder / 'X-VESA.COM')
    files['X-VESA.COM'] = hashlib.sha256(data).hexdigest()
    # The release is GPL-3.0; retain the unmodified source license in the cache.
    license_url = f'https://raw.githubusercontent.com/Marco-Pistella/X-VESA/{XVESA_REVISION}/LICENSE'
    fetch(license_url, '3bf2469479598343386b0388eba697581f7b06e54c1d9e727b833eb242d32cab',
          folder / 'XVESA-LICENSE')
    manifest = {'vgatest': {'version': '1.5', 'url': VGATEST_URL, 'sha256': VGATEST_SHA,
                           'license': 'GPL-3.0-or-later'},
                'xvesa': {'revision': XVESA_REVISION, 'url': XVESA_URL, 'sha256': XVESA_SHA,
                          'license': 'GPL-3.0'}, 'files': files}
    (folder / 'diagnostics.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/vga-diagnostics/tools')
    args = parser.parse_args()
    prepare(args.output)
    print(args.output.resolve())
