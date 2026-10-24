#!/usr/bin/env python3
"""Fetch checksum-pinned Debian trixie ARM64 netboot assets and stage a virtio ESP."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import urllib.request
from fat_stage import Writer

ROOT = Path(__file__).resolve().parents[1]
BASE = 'https://deb.debian.org/debian/dists/trixie/main/installer-arm64/current/images/'
ASSETS = {
    'linux': '9c37ace54ef7e1919411fd912e9f34d6982ddd7f4c8ef3c8aee56f4c4d70db67',
    'initrd.gz': 'f8f48fe31f84fca1a43c3d57c09cd1c2ee728549dd18a986463efd0305249451',
}
CMDLINE = 'initrd=\\INITRD.GZ console=ttyAMA0 earlycon=pl011,0x09000000 acpi=force'


def fetch(cache):
    cache.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for name, expected in ASSETS.items():
        url = BASE + 'netboot/debian-installer/arm64/' + name
        path = cache / name
        if not path.exists():
            temporary = path.with_suffix(path.suffix + '.download')
            try:
                with urllib.request.urlopen(url, timeout=60) as source, temporary.open('wb') as target:
                    while data := source.read(1024 * 1024):
                        target.write(data)
                if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
                    raise ValueError(f'Debian asset changed: {url}; review published SHA256SUMS before updating pins')
                temporary.rename(path)
            finally:
                temporary.unlink(missing_ok=True)
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Debian checksum mismatch: {path}')
        manifest[name] = {'url': url, 'sha256': expected, 'bytes': path.stat().st_size}
    (cache / 'download.json').write_text(json.dumps(manifest, indent=2) + '\n')


def create(path, cache, cmdline=CMDLINE):
    # 256 MiB sparse FAT16 ESP: enough for the uncompressed EFI kernel and initrd.
    start, sectors = 2048, 256 * 2048
    volume = sectors - start
    mbr = bytearray(512)
    mbr[446], mbr[450] = 0x80, 0xef
    struct.pack_into('<II', mbr, 454, start, volume)
    mbr[510:512] = b'\x55\xaa'
    boot = bytearray(512)
    boot[:11] = b'\xeb\x3c\x90MSDOS5.0'
    struct.pack_into('<HBHBHHBH', boot, 11, 512, 16, 1, 2, 512, 0, 0xf8, 128)
    struct.pack_into('<HHII', boot, 24, 63, 255, start, volume)
    boot[36:39] = b'\x80\x00\x29'
    struct.pack_into('<I', boot, 39, 0x5043454d)
    boot[43:54], boot[54:62] = b'DEBIAN BOOT', b'FAT16   '
    boot[510:512] = b'\x55\xaa'
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as output:
        output.truncate(sectors * 512)
        output.write(mbr)
        output.seek(start * 512)
        output.write(boot)
        for fat in range(2):
            output.seek((start + 1 + fat * 128) * 512)
            output.write(b'\xf8\xff\xff\xff')
    writer = Writer(path)
    try:
        files = {'LINUX.EFI': (cache / 'linux').read_bytes(),
                 'INITRD.GZ': (cache / 'initrd.gz').read_bytes(),
                 'STARTUP.NSH': ('fs0:\\LINUX.EFI ' + cmdline + '\r\n').encode('ascii')}
        for name, data in files.items():
            writer.root_entry(writer.entry(name, 32, writer.allocate(data), len(data)))
        writer.commit()
    finally:
        writer.f.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=ROOT / 'build/debian-arm64')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/debian-arm64/netboot.raw')
    parser.add_argument('--cmdline', default=CMDLINE)
    args = parser.parse_args()
    fetch(args.cache)
    create(args.output, args.cache, args.cmdline)
    print(args.output)


if __name__ == '__main__':
    main()
