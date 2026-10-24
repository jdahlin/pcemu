#!/usr/bin/env python3
"""Stage Netscape 3.04 in a new compressed Win95 overlay, with pinned readback.

Cold-boot the child disk. Never resume the parent's RAM against offline disk edits.
The Windows 95 NE2000 and TCP/IP drivers come from the user's C:\\WIN95 files.
"""
import argparse
import hashlib
import struct
import subprocess
import urllib.request
from pathlib import Path
from cow_file import CowFile
from fat_stage import Writer

ROOT = Path(__file__).resolve().parent.parent
URL = 'https://www.devili.iki.fi/pub/browsers/Netscape/win32/n32e304p.exe'
SHA256 = '593bde09d6f59e8668a19852388b007186edecffd04b8a356b8776a3be6fc3ef'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('parent', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.destination.exists():
        parser.error('Choose a new private destination; it already exists')
    archive = ROOT / 'media/network/n32e304p.exe'
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        data = urllib.request.urlopen(URL, timeout=60).read()
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise ValueError('Netscape download checksum mismatch')
        archive.write_bytes(data)
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError('Netscape archive checksum mismatch')
    subprocess.run(['make', '-s', 'build/disk-tool'], cwd=ROOT, check=True)
    subprocess.run([str(ROOT/'build/disk-tool'), 'overlay', str(args.destination.resolve()),
                    str(args.parent.resolve())], check=True)
    with CowFile(args.destination) as handle:
        fat = Writer(handle)
        fat.put_directory('NETSETUP', {'NETSCAPE.EXE': data})
        entry = next(e for e in fat.entries() if e[1] == 'NETSETUP   ')
        directory = fat.read_file(entry[2], fat.cluster_bytes * len(list(fat.chain(entry[2]))))
        item = directory[64:96]
        cluster, size = struct.unpack_from('<H', item, 26)[0], struct.unpack_from('<I', item, 28)[0]
        if fat.read_file(cluster, size) != data:
            raise ValueError('Guest installer readback mismatch')
    print(f'Verified C:\\NETSETUP\\NETSCAPE.EXE; overlay {args.destination.stat().st_size:,} bytes')
    print('Cold boot, add Novell/Anthem > NE2000 Compatible, I/O 300-31F, IRQ 10.')
    print('Add Microsoft TCP/IP, automatic address; see docs/networking.md.')

if __name__ == '__main__':
    main()
