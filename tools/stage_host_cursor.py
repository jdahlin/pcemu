#!/usr/bin/env python3
"""Add PCEmu's cursor DDI segment to the pinned Win95 display driver.

The original code and relocation tables remain intact. Only exported cursor
ordinals 102-104 are redirected to an appended relocation-free NE segment.
Use exclusively with PCEmu; this adapter suppresses the software cursor.
Parent must be cleanly shut down and immutable while its child is in use.
"""
import argparse
import hashlib
import struct
import subprocess
from pathlib import Path
from zipfile import ZipFile
from cow_file import CowFile
from fat_stage import Writer
from stage_display_driver import ROOT, SHA256

def adapt_driver(original, code):
    b = bytearray(original)
    n = struct.unpack_from('<I', b, 60)[0]
    if b[n:n+2] != b'NE':
        raise ValueError('Expected NE driver')
    count = struct.unpack_from('<H', b, n+28)[0]
    table = struct.unpack_from('<H', b, n+34)[0]
    shift = struct.unpack_from('<H', b, n+50)[0]
    if count != 2 or shift != 1:
        raise ValueError('Unexpected pinned driver segment layout')
    entries = bytes(b[n+table:n+table+count*8])
    offsets = struct.unpack_from('<HHH', code, len(code)-6)
    code = code[:-6]
    # Grow the segment table in place. NE loaders require its tables in the
    # header region; adjust all following table and file-sector references.
    insert = n+table+count*8
    b[insert:insert] = bytes(8)
    for field in (4, 36, 38, 40, 42):
        value = struct.unpack_from('<H', b, n+field)[0]
        if value >= table+count*8:
            struct.pack_into('<H', b, n+field, value+8)
    nonresident = struct.unpack_from('<I', b, n+44)[0]
    struct.pack_into('<I', b, n+44, nonresident+8)
    for i in range(count):
        at = n+table+i*8
        sector = struct.unpack_from('<H', b, at)[0]
        struct.pack_into('<H', b, at, sector+(8 >> shift))
    resource = n+struct.unpack_from('<H', b, n+36)[0]
    rshift = struct.unpack_from('<H', b, resource)[0]
    if 8 % (1 << rshift):
        raise ValueError('Unsupported resource alignment')
    p = resource+2
    while struct.unpack_from('<H', b, p)[0]:
        number = struct.unpack_from('<H', b, p+2)[0]
        p += 8
        for _ in range(number):
            sector = struct.unpack_from('<H', b, p)[0]
            struct.pack_into('<H', b, p, sector+(8 >> rshift))
            p += 12
    b.extend(b'\0' * (-len(b) % (1 << shift)))
    code_at = len(b)
    b.extend(code)
    struct.pack_into('<HHHH', b, insert, code_at >> shift, len(code), 0x50, len(code))
    struct.pack_into('<H', b, n+28, count+1)
    at, length = struct.unpack_from('<HH', b, n+4)
    p, ordinal, changed = n+at, 1, []
    while p < n+at+length:
        num, kind = b[p:p+2]
        p += 2
        if not num:
            break
        if not kind:
            ordinal += num
            continue
        for _ in range(num):
            if ordinal in (102, 103, 104):
                if kind != 255 or b[p+3] != 1:
                    raise ValueError('Unexpected cursor export')
                b[p+3] = 3
                struct.pack_into('<H', b, p+4, offsets[ordinal-102])
                changed.append(ordinal)
            p += 6 if kind == 255 else 3
            ordinal += 1
    if changed != [102, 103, 104]:
        raise ValueError('Missing cursor exports')
    return bytes(b)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('parent', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.destination.exists():
        parser.error('Destination already exists')
    archive = ROOT/'media/drivers/vmdisp9x-2d.zip'
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError('Pinned driver checksum mismatch')
    binary = ROOT/'build/host-cursor.bin'
    subprocess.run(['nasm', '-f', 'bin', str(ROOT/'guest/host_cursor.asm'), '-o', str(binary)], check=True)
    with ZipFile(archive) as z:
        original = z.read('qemumini.drv')
        driver = adapt_driver(original, binary.read_bytes())
    subprocess.run([str(ROOT/'build/disk-tool'), 'overlay', str(args.destination), str(args.parent)], check=True)
    with CowFile(args.destination) as handle:
        fat = Writer(handle)
        # Replace only the stopped child's installed driver; immutable parent
        # retains the original. Allocate the new file before releasing its chain.
        directory_at = fat.rootbase
        directory_size = fat.nroot * 32
        for name in ('WINDOWS    ', 'SYSTEM     ', 'QEMUMINI'+'DRV'):
            fat.f.seek(directory_at)
            directory = fat.f.read(directory_size)
            offset = next(i for i in range(0, len(directory), 32)
                          if directory[i:i+11] == name.encode())
            cluster = struct.unpack_from('<H', directory, offset+26)[0]
            if name == 'QEMUMINIDRV':
                old_size = struct.unpack_from('<I', directory, offset+28)[0]
                if fat.read_file(cluster, old_size) != original:
                    raise ValueError('Installed display driver differs from pinned qemumini.drv')
                old_chain = list(fat.chain(cluster))
                new_cluster = fat.allocate(driver)
                entry = bytearray(directory[offset:offset+32])
                struct.pack_into('<H', entry, 26, new_cluster)
                struct.pack_into('<I', entry, 28, len(driver))
                fat.f.seek(directory_at+offset)
                fat.f.write(entry)
                for c in old_chain:
                    struct.pack_into('<H', fat.fat, c*2, 0)
                fat.commit()
                if fat.read_file(new_cluster, len(driver)) != driver:
                    raise ValueError('Installed driver readback mismatch')
                break
            chain = list(fat.chain(cluster))
            # Windows' small directories occupy contiguous clusters in this
            # fixture. Reject fragmented layouts instead of overwriting them.
            if chain != list(range(chain[0], chain[0]+len(chain))):
                raise ValueError('Fragmented installed-driver directory')
            directory_at = fat.database+(cluster-2)*fat.cluster_bytes
            directory_size = len(chain)*fat.cluster_bytes
    print('Installed and verified native cursor display DDI in', args.destination)

if __name__ == '__main__':
    main()
