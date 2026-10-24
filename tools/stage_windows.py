#!/usr/bin/env python3
"""Copy the user's Windows install floppies to C:\\WINSETUP in the working FAT16 disk."""
from pathlib import Path
import struct
from prepare_media import FAT


def main():
    disk = FAT(Path('media/dos.raw'))
    if disk.bits != 16:
        raise ValueError('Working disk must be FAT16')
    cluster_bytes = disk.spc * 512

    def allocate(data):
        count = (len(data) + cluster_bytes - 1) // cluster_bytes
        free = [c for c in range(2, len(disk.fat) // 2) if struct.unpack_from('<H', disk.fat, c * 2)[0] == 0][:count]
        if len(free) != count:
            raise ValueError('Disk full')
        for i, c in enumerate(free):
            struct.pack_into('<H', disk.fat, c * 2, free[i+1] if i+1 < count else 0xffff)
            disk.f.seek(disk.database + (c - 2) * cluster_bytes)
            disk.f.write(data[i*cluster_bytes:(i+1)*cluster_bytes].ljust(cluster_bytes, b'\0'))
        return free[0] if free else 0

    def entry(name, attr, cluster, size):
        e = bytearray(32)
        e[:11] = name.encode('ascii')
        e[11] = attr
        # DOS-compatible date 1994-01-01.
        struct.pack_into('<H', e, 24, (14 << 9) | 33)
        struct.pack_into('<H', e, 26, cluster)
        struct.pack_into('<I', e, 28, size)
        return e

    for _, name, _, _ in disk.entries():
        if name == 'WINSETUP   ':
            print('C:\\WINSETUP already staged')
            return
    files = {}
    for path in sorted(Path('media').rglob('DISK*.IMG')):
        floppy = FAT(path)
        for off, name, c, size in floppy.entries():
            floppy.f.seek(floppy.rootbase + off + 11)
            attr = floppy.f.read(1)[0]
            if not attr & 0x18:
                files[name] = floppy.read_file(c, size)
        floppy.f.close()
    if 'SETUP   EXE' not in files:
        raise ValueError('Windows setup files not found')
    # Allocate the directory first so its dot entry can refer to itself.
    dir_size = ((len(files) + 3) * 32 + cluster_bytes - 1) // cluster_bytes * cluster_bytes
    dir_cluster = allocate(bytes(dir_size))
    directory = entry('.          ', 0x10, dir_cluster, 0) + entry('..         ', 0x10, 0, 0)
    for name, data in sorted(files.items()):
        directory += entry(name, 0x20, allocate(data), len(data))
    directory = directory.ljust(dir_size, b'\0')
    for i, c in enumerate(disk.chain(dir_cluster)):
        disk.f.seek(disk.database + (c - 2) * cluster_bytes)
        disk.f.write(directory[i*cluster_bytes:(i+1)*cluster_bytes])
    disk.f.seek(disk.rootbase)
    root = disk.f.read(disk.nroot * 32)
    slot = next(off for off in range(0, len(root), 32) if root[off] in (0, 0xe5))
    for i in range(disk.nfats):
        disk.f.seek(disk.fatbase + i * disk.spf * 512)
        disk.f.write(disk.fat)
    disk.f.seek(disk.rootbase + slot)
    disk.f.write(entry('WINSETUP   ', 0x10, dir_cluster, 0))
    disk.f.close()
    print(f'Staged {len(files)} files in C:\\WINSETUP ({sum(map(len, files.values())):,} bytes)')

if __name__ == '__main__':
    main()
