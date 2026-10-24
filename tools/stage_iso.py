#!/usr/bin/env python3
"""Stage ISO9660 8.3 files into an offline, disposable FAT16 raw disk copy.

Reads file payloads directly from the ISO without an extracted directory copy.
Use disk-tool import-diff afterwards to retain only compressed changed blocks.
"""
import argparse
import struct
from pathlib import Path
from fat_stage import Writer


class ISO:
    def __init__(self, path):
        self.f = path.open('rb')
        self.f.seek(16 * 2048)
        volume = self.f.read(2048)
        if volume[:7] != b'\x01CD001\x01':
            raise ValueError('ISO9660 primary descriptor required')
        self.root = self.record(volume[156:190])

    @staticmethod
    def record(record):
        return (record[33:33 + record[32]].decode('ascii').split(';')[0],
                struct.unpack_from('<I', record, 2)[0],
                struct.unpack_from('<I', record, 10)[0], bool(record[25] & 2))

    def read(self, record):
        self.f.seek(record[1] * 2048)
        data = self.f.read(record[2])
        if len(data) != record[2]:
            raise ValueError('Truncated ISO payload')
        return data

    def children(self, record):
        data, at = self.read(record), 0
        while at < len(data):
            size = data[at]
            if not size:
                at = (at // 2048 + 1) * 2048
                continue
            child = self.record(data[at:at + size])
            if child[0] not in ('\x00', '\x01'):
                # Reject names requiring aliases rather than silently changing setup paths.
                parts = child[0].split('.')
                if not 1 <= len(parts) <= 2 or not 1 <= len(parts[0]) <= 8 or (len(parts) == 2 and not 1 <= len(parts[1]) <= 3):
                    raise ValueError('ISO name is not 8.3: ' + child[0])
                Writer.entry(child[0], 16 if child[3] else 32, 0, 0)
                yield child
            at += size


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('iso', type=Path)
    p.add_argument('disk', type=Path)
    p.add_argument('--directory', default='OFFICE97')
    p.add_argument('--verify-only', action='store_true')
    a = p.parse_args()
    iso, disk = ISO(a.iso), Writer(a.disk)
    target = Writer.entry(a.directory, 16, 0, 0)[:11].decode('ascii')
    existing = next((e for e in disk.entries() if e[1] == target), None)
    if existing and not a.verify_only:
        p.error('Destination directory already exists')
    if a.verify_only and not existing:
        p.error('Destination directory missing')
    files, total = 0, 0
    verified = 0

    def directory(record, parent):
        nonlocal files, total
        children = list(iso.children(record))
        size = ((len(children) + 3) * 32 + disk.cluster_bytes - 1) // disk.cluster_bytes * disk.cluster_bytes
        cluster = disk.allocate(bytes(size))
        entries = disk.entry('.          ', 16, cluster, 0) + disk.entry('..         ', 16, parent, 0)
        for child in children:
            if child[3]:
                start = directory(child, cluster)
            else:
                start = disk.allocate(iso.read(child))
                files += 1
                total += child[2]
            entries += disk.entry(child[0], 16 if child[3] else 32, start, 0 if child[3] else child[2])
        entries = entries.ljust(size, b'\0')
        for index, c in enumerate(disk.chain(cluster)):
            disk.f.seek(disk.database + (c - 2) * disk.cluster_bytes)
            disk.f.write(entries[index * disk.cluster_bytes:(index + 1) * disk.cluster_bytes])
        return cluster

    if a.verify_only:
        start = existing[2]
    else:
        start = directory(iso.root, 0)
        disk.root_entry(disk.entry(a.directory, 16, start, 0))
        disk.commit()
    # Read every staged directory and payload back through the FAT chains.
    def verify(record, cluster):
        nonlocal verified
        data = disk.read_file(cluster, sum(1 for _ in disk.chain(cluster)) * disk.cluster_bytes)
        found = {}
        for at in range(0, len(data), 32):
            entry = data[at:at + 32]
            if not entry[0]:
                break
            found[entry[:11]] = entry
        for child in iso.children(record):
            key = bytes(disk.entry(child[0], 16 if child[3] else 32, 0, 0)[:11])
            entry = found[key]
            c = struct.unpack_from('<H', entry, 26)[0]
            if child[3]:
                verify(child, c)
            else:
                if disk.read_file(c, child[2]) != iso.read(child):
                    raise ValueError('Read-back differs: ' + child[0])
                verified += 1
    verify(iso.root, start)
    disk.f.close()
    iso.f.close()
    print(f'Verified {verified} files in C:\\{a.directory}; staged {files} files, {total:,} bytes')


if __name__ == '__main__':
    main()
