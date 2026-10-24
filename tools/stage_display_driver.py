#!/usr/bin/env python3
"""Stage the pinned VMDisp9x guest driver in a new compressed FAT16 disk overlay.

Use a cleanly shut down disk or an immutable checkpoint disk as the parent.
No raw disk copy or host filesystem mount is needed.
"""
import argparse
import hashlib
import struct
import subprocess
import urllib.request
from pathlib import Path
from zipfile import ZipFile
from cow_file import CowFile
from fat_stage import Writer

ROOT = Path(__file__).resolve().parent.parent
URL = 'https://github.com/JHRobotics/vmdisp9x/releases/download/v1.2025.0.119/vmdisp9x-1.2025.0.119b-driver-2d.zip'
SHA256 = 'e0d698a6089347a6a619ed439c092f247392fbafac6b342f4651c1380911fb23'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('parent', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.destination.exists():
        parser.error('Destination already exists; choose a new private overlay')
    archive = ROOT / 'media/drivers/vmdisp9x-2d.zip'
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        data = urllib.request.urlopen(URL, timeout=60).read()
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise ValueError('Downloaded driver checksum mismatch')
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError('Driver archive checksum mismatch')
    subprocess.run(['make', '-s', 'build/disk-tool'], cwd=ROOT, check=True)
    subprocess.run([str(ROOT/'build/disk-tool'), 'overlay', str(args.destination), str(args.parent)], check=True)
    with ZipFile(archive) as driver:
        files = {name: driver.read(name) for name in driver.namelist()}
    with CowFile(args.destination) as handle:
        fat = Writer(handle)
        fat.put_directory('DISPLAY', files)
        entry = next(e for e in fat.entries() if e[1] == 'DISPLAY    ')
        directory = fat.read_file(entry[2], fat.cluster_bytes * len(list(fat.chain(entry[2]))))
        verified = 0
        for offset in range(64, len(directory), 32):
            item = directory[offset:offset+32]
            if not item[0]:
                break
            name = item[:8].decode().rstrip() + '.' + item[8:11].decode().rstrip()
            cluster, size = struct.unpack_from('<H', item, 26)[0], struct.unpack_from('<I', item, 28)[0]
            if fat.read_file(cluster, size) != files[name.lower()]:
                raise ValueError('Guest driver readback mismatch: '+name)
            verified += 1
        if verified != len(files):
            raise ValueError('Guest driver directory incomplete')
    print(f'Verified {verified} files in C:\\DISPLAY; overlay {args.destination.stat().st_size:,} bytes')
    print('In Windows: Display Properties > Settings > Advanced Properties > Adapter > Change > Have Disk.')
    print('Use C:\\DISPLAY, show all devices, choose QEMU STD VGA PCI Adapter (compatible DISPI interface).')
    print('If file copying asks for a source, enter C:\\DISPLAY again. Restart Windows after installation.')

if __name__ == '__main__':
    main()
