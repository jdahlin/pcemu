#!/usr/bin/env python3
"""Create a small sparse FAT16 ESP for virtio/UEFI integration tests."""
import argparse
from pathlib import Path
import struct
from fat_stage import Writer


def create(path, efi=None):
    sectors = 64 * 2048
    start = 2048
    volume = sectors - start
    mbr = bytearray(512)
    mbr[446] = 0x80
    mbr[450] = 0xef
    struct.pack_into('<II', mbr, 454, start, volume)
    mbr[510:512] = b'\x55\xaa'
    boot = bytearray(512)
    boot[:11] = b'\xeb\x3c\x90MSDOS5.0'
    struct.pack_into('<HBHBHHBH', boot, 11, 512, 4, 1, 2, 512, 0, 0xf8, 128)
    struct.pack_into('<HHII', boot, 24, 63, 255, start, volume)
    boot[36:39] = b'\x80\x00\x29'
    struct.pack_into('<I', boot, 39, 0x5043454d)
    boot[43:54] = b'PCEMU UEFI '
    boot[54:62] = b'FAT16   '
    boot[510:512] = b'\x55\xaa'
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
        contents = b'PCEMU_VIRTIO_DISK_OK\r\n'
        writer.root_entry(writer.entry('PROBE.TXT', 32, writer.allocate(contents), len(contents)))
        if efi:
            # Nested EFI/BOOT directories, all canonical 8.3 names.
            parent = writer.allocate(bytes(writer.cluster_bytes))
            child = writer.allocate(bytes(writer.cluster_bytes))
            payload = Path(efi).read_bytes()
            file_cluster = writer.allocate(payload)
            for cluster, up, entries in (
                (parent, 0, [writer.entry('BOOT', 16, child, 0)]),
                (child, parent, [writer.entry('BOOTAA64.EFI', 32, file_cluster, len(payload))]),
            ):
                data = (writer.entry('.          ', 16, cluster, 0) +
                        writer.entry('..         ', 16, up, 0) + b''.join(entries))
                writer.f.seek(writer.database + (cluster - 2) * writer.cluster_bytes)
                writer.f.write(data.ljust(writer.cluster_bytes, b'\0'))
            writer.root_entry(writer.entry('EFI', 16, parent, 0))
        writer.commit()
    finally:
        writer.f.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--efi', type=Path)
    args = parser.parse_args()
    create(args.output, args.efi)
    print(args.output)
