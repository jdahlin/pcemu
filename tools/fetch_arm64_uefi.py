#!/usr/bin/env python3
"""Fetch a pinned EDK II ARM64 firmware build; no QEMU executable is used."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
URL = ('https://deb.debian.org/debian/pool/main/e/edk2/'
       'qemu-efi-aarch64_2025.02-8+deb13u1_all.deb')
SHA256 = 'a00b2411a79c8aeafd95a7c868ac3cd1aab592f1af9fff965f02d81a625276ed'


def main():
    cache = ROOT / 'build/arm64-fw'
    cache.mkdir(parents=True, exist_ok=True)
    record = cache / 'download.json'
    if record.exists():
        manifest = json.loads(record.read_text())
        valid = manifest.get('source') == URL and manifest.get('packageSHA256') == SHA256
        entries = manifest.get('files', [])
        valid = valid and {entry.get('name') for entry in entries} == {
            'edk2-arm64.bin', 'edk2-arm64-vars.bin', 'copyright.txt', 'README.Debian'}
        for entry in entries:
            name = entry['name']
            path = ROOT / 'roms' / name if name.endswith('.bin') else cache / name
            valid = valid and path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256']
        if valid:
            print('Pinned ARM64 EDK II firmware already verified')
            return
    package = cache / 'edk2.deb'
    if not package.exists():
        with urllib.request.urlopen(URL, timeout=60) as source:
            data = source.read(8 * 1024 * 1024)
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise RuntimeError('EDK II Debian package checksum mismatch')
        package.write_bytes(data)
    data = package.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise RuntimeError('EDK II cached package checksum mismatch')
    if data[:8] != b'!<arch>\n':
        raise RuntimeError('Not a Debian ar archive')
    pos = 8
    while pos < len(data):
        header = data[pos:pos + 60]
        size = int(header[48:58])
        start = pos + 60
        if header[:16].strip() == b'data.tar.xz':
            payload = data[start:start + size]
            break
        pos = start + size + size % 2
    else:
        raise RuntimeError('Missing Debian payload')
    wanted = {
        './usr/share/qemu-efi-aarch64/QEMU_EFI.fd': ('edk2-arm64.bin', None),
        './usr/share/AAVMF/AAVMF_VARS.fd': ('edk2-arm64-vars.bin', 0xc0000),
        './usr/share/doc/qemu-efi-aarch64/copyright': ('copyright.txt', None),
        './usr/share/doc/qemu-efi-aarch64/README.Debian': ('README.Debian', None),
    }
    manifest = {'source': URL, 'packageSHA256': SHA256, 'files': []}
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r|xz') as archive:
        for member in archive:
            if member.name not in wanted:
                continue
            name, limit = wanted.pop(member.name)
            content = archive.extractfile(member).read(limit or member.size)
            destination = ROOT / 'roms' / name if name.endswith('.bin') else cache / name
            if destination.exists() and destination.read_bytes() != content:
                raise RuntimeError(f'Refusing to overwrite different firmware: {destination}')
            destination.write_bytes(content)
            manifest['files'].append({'name': name, 'bytes': len(content),
                                      'sha256': hashlib.sha256(content).hexdigest()})
            print(f'{destination}: {len(content):,} bytes', flush=True)
            if not wanted:
                break
    if wanted:
        raise RuntimeError('Missing firmware files')
    (cache / 'download.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
