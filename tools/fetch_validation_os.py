#!/usr/bin/env python3
"""Fetch only the ARM64 Validation OS WIM from Microsoft's official ISO.

Dependency: python3 -m pip install --target build/media-tools pycdlib==1.15.0
The ISO is read using verified HTTP byte ranges; it is never stored locally.
"""
import argparse
import functools
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/media-tools'))
import pycdlib

SOURCE = ('https://software-static.download.prss.microsoft.com/dbazure/'
          '26100.9549.260917-1814.ge_release_svc_prod3_arm64fre_en-us_VALIDATIONOS.iso')
LICENSE = 'https://learn.microsoft.com/en-us/legal/windows/hardware/validation-os-license'
FILES = {'/ValidationOS.wim': 'ValidationOS.wim', '/license.rtf': 'license.rtf',
         '/GenImage/readme.txt': 'GenImage-readme.txt'}


class RemoteISO(io.RawIOBase):
    block_size = 1024 * 1024

    def __init__(self):
        super().__init__()
        self.pos = 0
        self.fetched = 0
        with urllib.request.urlopen(urllib.request.Request(SOURCE, method='HEAD'), timeout=30) as f:
            self.size = int(f.headers['Content-Length'])
            self.etag = f.headers.get('ETag')

    def seekable(self):
        return True

    def readable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=0):
        if whence not in (0, 1, 2):
            raise ValueError('Invalid seek mode')
        pos = offset if whence == 0 else self.pos + offset if whence == 1 else self.size + offset
        if pos < 0:
            raise ValueError('Negative seek')
        self.pos = pos
        return pos

    @functools.lru_cache(maxsize=8)
    def block(self, index):
        begin = index * self.block_size
        end = min(self.size, begin + self.block_size) - 1
        headers = {'Range': f'bytes={begin}-{end}', 'Accept-Encoding': 'identity'}
        if self.etag:
            headers['If-Match'] = self.etag
        with urllib.request.urlopen(urllib.request.Request(SOURCE, headers=headers), timeout=60) as f:
            expected = f'bytes {begin}-{end}/{self.size}'
            if f.status != 206 or f.headers.get('Content-Range') != expected:
                raise RuntimeError('Server did not honor exact ISO byte range; refusing full download')
            data = f.read(self.block_size + 1)
        if len(data) != end - begin + 1:
            raise RuntimeError('Truncated HTTP range')
        self.fetched += len(data)
        if self.fetched > 512 * 1024 * 1024:
            raise RuntimeError('Exceeded 512 MiB transfer budget')
        return data

    def read(self, size=-1):
        if size < 0 or size > 2 * self.block_size:
            raise ValueError('Unbounded remote ISO read refused')
        end = min(self.size, self.pos + size)
        out = bytearray()
        while self.pos < end:
            block = self.block(self.pos // self.block_size)
            offset = self.pos % self.block_size
            length = min(end - self.pos, len(block) - offset)
            out.extend(block[offset:offset + length])
            self.pos += length
        return bytes(out)


def inspect_wim(path):
    with path.open('rb') as f:
        header = f.read(208)
        if header[:8] != b'MSWIM\0\0\0':
            raise RuntimeError('Downloaded payload is not a WIM')
        xml_size = struct.unpack_from('<Q', header, 72)[0] & ((1 << 56) - 1)
        xml_offset = struct.unpack_from('<Q', header, 80)[0]
        if xml_size > 1024 * 1024 or xml_offset + xml_size > path.stat().st_size:
            raise RuntimeError('Invalid WIM XML resource')
        f.seek(xml_offset)
        xml = ET.fromstring(f.read(xml_size))
    images = []
    for image in xml.findall('IMAGE'):
        windows = image.find('WINDOWS')
        arch = windows.findtext('ARCH') if windows is not None else None
        if arch != '12':
            raise RuntimeError(f'Expected ARM64 WIM (architecture 12), got {arch}')
        images.append({'name': image.findtext('NAME'), 'architecture': 'ARM64',
                       'build': windows.findtext('VERSION/BUILD'),
                       'revision': windows.findtext('VERSION/SPBUILD')})
    if not images:
        raise RuntimeError('WIM contains no images')
    return images


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, default=ROOT / 'media/validation-os-arm64')
    args = parser.parse_args()
    # Never overwrite a prior download or a user-maintained VM directory.
    args.destination.mkdir(parents=True, exist_ok=False)
    remote = RemoteISO()
    iso = pycdlib.PyCdlib()
    iso.open_fp(remote)
    manifest = {'source': SOURCE, 'sourceISOBytes': remote.size, 'sourceETag': remote.etag,
                'license': LICENSE, 'pycdlibVersion': '1.15.0', 'files': []}
    for source, name in FILES.items():
        target = args.destination / name
        expected = iso.get_record(udf_path=source).get_data_length()
        if expected > 300 * 1024 * 1024:
            raise RuntimeError('Unexpectedly large payload')
        print(f'Fetching {source}: {expected:,} bytes', flush=True)
        partial = target.with_suffix(target.suffix + '.part')
        with partial.open('xb') as f:
            iso.get_file_from_iso_fp(f, udf_path=source, blocksize=remote.block_size)
        if partial.stat().st_size != expected:
            raise RuntimeError('ISO extraction size mismatch')
        digest = hashlib.sha256()
        with partial.open('rb') as f:
            for block in iter(lambda: f.read(1024 * 1024), b''):
                digest.update(block)
        partial.rename(target)
        manifest['files'].append({'name': name, 'isoPath': source, 'bytes': expected,
                                  'sha256': digest.hexdigest()})
    manifest['images'] = inspect_wim(args.destination / 'ValidationOS.wim')
    manifest['downloadedBytes'] = remote.fetched
    (args.destination / 'download.json').write_text(json.dumps(manifest, indent=2) + '\n')
    iso.close()
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
