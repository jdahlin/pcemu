#!/usr/bin/env python3
"""Stage the pinned QEMouse Win9x guest driver in a private compressed child.

The parent must be stopped and immutable. Driver code is an external GPL-2.0
guest asset, never linked into the emulator.
"""
import argparse
import hashlib
import subprocess
import urllib.request
from pathlib import Path
from zipfile import ZipFile
from cow_file import CowFile
from fat_stage import Writer

ROOT = Path(__file__).resolve().parents[1]
URL = 'https://github.com/qemus/qemouse/releases/download/v1.03/qemouse.zip'
SHA256 = '1970cb19bf99558acdc88c110cfc9a5bfe54a58c927118b1970048c229a6ad71'
SOURCE = 'https://github.com/qemus/qemouse/tree/v1.03'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('parent', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.destination.exists():
        parser.error('Destination already exists')
    archive = ROOT / 'media/drivers/qemouse-1.03.zip'
    archive.parent.mkdir(parents=True, exist_ok=True)
    data = archive.read_bytes() if archive.exists() else urllib.request.urlopen(URL, timeout=60).read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError('QEMouse checksum mismatch')
    if not archive.exists():
        archive.write_bytes(data)
    subprocess.run([str(ROOT / 'tools/build.sh'), 'disk-tool'], cwd=ROOT, check=True)
    with ZipFile(archive) as package:
        files = {name: package.read(name) for name in ('qemouse.drv', 'qemouse.inf')}
    files['source.txt'] = ('QEMouse v1.03, GPL-2.0\r\nSource and license: ' + SOURCE + '\r\n').encode()
    subprocess.run([str(ROOT / 'build/disk-tool'), 'overlay', str(args.destination), str(args.parent)], check=True)
    with CowFile(args.destination) as handle:
        fat = Writer(handle)
        fat.put_directory('QEMOUSE', files)
        entry = next(e for e in fat.entries() if e[1] == 'QEMOUSE    ')
        directory = fat.read_file(entry[2], fat.cluster_bytes * len(list(fat.chain(entry[2]))))
        import struct
        for off in range(64, 64 + len(files) * 32, 32):
            item = directory[off:off+32]
            name = item[:8].decode().strip().lower() + '.' + item[8:11].decode().strip().lower()
            cluster, size = struct.unpack_from('<H', item, 26)[0], struct.unpack_from('<I', item, 28)[0]
            if fat.read_file(cluster, size) != files[name]:
                raise ValueError('Guest mouse driver readback mismatch')
    print('Verified QEMouse files in C:\\QEMOUSE. Install qemouse.inf in Windows, then reboot.')

if __name__ == '__main__':
    main()
