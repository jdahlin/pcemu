#!/usr/bin/env python3
"""Download full Windows 11 ARM64 media with Microsoft's published SHA-256.

Obtain a fresh English (United States) link from the official ARM64 download
page. Store it as {"url": "..."} in an ignored JSON file and pass --request.
The default checksum is for the inspected 26H2 build 26300.9457 media.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from concurrent.futures import ThreadPoolExecutor
import time
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_SHA256 = '8992f2d2cfd2fa647e579e9eb31fc21f34da8333c29b6170fde17d2ed6623e9c'
PAGE = 'https://www.microsoft.com/en-us/software-download/windows11arm64'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--request', type=Path, required=True)
    parser.add_argument('--destination', type=Path, default=ROOT / 'media/windows11-arm64')
    parser.add_argument('--sha256', default=EXPECTED_SHA256)
    args = parser.parse_args()
    expected = args.sha256.lower()
    if len(expected) != 64 or any(c not in '0123456789abcdef' for c in expected):
        parser.error('--sha256 must be a SHA-256 hex digest from Microsoft')
    url = json.loads(args.request.read_text())['url']
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != 'https' or parsed.hostname != 'software.download.prss.microsoft.com'
            or not parsed.path.lower().endswith('.iso')):
        raise RuntimeError('Expected official Microsoft ISO URL')
    args.destination.mkdir(parents=True, exist_ok=True)
    target = args.destination / Path(parsed.path).name
    partial = target.with_suffix('.iso.part')
    metadata = target.with_suffix('.iso.partial.json')
    if target.exists():
        raise RuntimeError('Destination ISO already exists; refusing overwrite')
    with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=60) as response:
        size = int(response.headers['Content-Length'])
        etag = response.headers.get('ETag')
    if not 1024 ** 3 <= size <= 10 * 1024 ** 3 or not etag:
        raise RuntimeError('Unexpected ISO size or missing ETag')
    identity = {'source': urllib.parse.urlunsplit(parsed._replace(query='', fragment='')),
                'bytes': size, 'etag': etag, 'expectedSha256': expected}
    if partial.exists():
        if not metadata.exists() or json.loads(metadata.read_text()) != identity:
            raise RuntimeError('Partial download belongs to different media')
    if shutil.disk_usage(args.destination).free < size - (partial.stat().st_size if partial.exists() else 0) + 1024 ** 3:
        raise RuntimeError('Insufficient free disk space for ISO plus 1 GiB reserve')
    metadata.write_text(json.dumps(identity, indent=2) + '\n')
    print(f'Downloading {target.name}: {size:,} bytes', flush=True)
    def chunk(begin, end):
        headers = {'Range': f'bytes={begin}-{end}', 'If-Match': etag, 'Accept-Encoding': 'identity'}
        for attempt in range(3):
            try:
                with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as response:
                    if response.status != 206 or response.headers.get('Content-Range') != f'bytes {begin}-{end}/{size}':
                        raise RuntimeError('Server did not honor the exact ISO range')
                    data = response.read(end - begin + 2)
                if len(data) != end - begin + 1:
                    raise RuntimeError('Truncated ISO range')
                return data
            except (OSError, RuntimeError):
                if attempt == 2:
                    raise
                time.sleep(1)
    offset = partial.stat().st_size if partial.exists() else 0
    if offset > size:
        raise RuntimeError('Partial download exceeds source size')
    # Four bounded in-flight chunks; commit in order so a partial is always a prefix.
    chunk_bytes = 16 * 1024 * 1024
    with partial.open('ab') as stream, ThreadPoolExecutor(max_workers=4) as pool:
        while offset < size:
            ranges = [(begin, min(begin + chunk_bytes, size) - 1)
                      for begin in range(offset, min(offset + 4 * chunk_bytes, size), chunk_bytes)]
            pending = [pool.submit(chunk, begin, end) for begin, end in ranges]
            for result in pending:
                data = result.result()
                stream.write(data)
                stream.flush()
                offset += len(data)
            print(f'{offset:,} / {size:,} bytes', flush=True)
    if partial.stat().st_size != size:
        raise RuntimeError('Downloaded ISO size mismatch')
    digest = hashlib.sha256()
    with partial.open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            digest.update(block)
    if digest.hexdigest() != expected:
        raise RuntimeError('ISO does not match Microsoft published SHA-256')
    partial.rename(target)
    report = {**identity, 'sha256': digest.hexdigest(), 'sha256Verified': True,
              'checksumSource': PAGE, 'path': str(target)}
    (args.destination / 'download.json').write_text(json.dumps(report, indent=2) + '\n')
    metadata.unlink()
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
