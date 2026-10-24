#!/usr/bin/env python3
"""Convert the user's dynamic VHD to a private sparse raw disk. No emulator dependency."""
import argparse
import os
from pathlib import Path
import struct


def convert(src, dst):
    with src.open('rb') as f:
        f.seek(-512, 2)
        footer = f.read(512)
        if footer[:8] != b'conectix':
            raise ValueError('Not a VHD')
        size, = struct.unpack_from('>Q', footer, 48)
        kind, = struct.unpack_from('>I', footer, 60)
        print('VHD geometry:', struct.unpack_from('>HBB', footer, 56))
        with dst.open('xb') as out:
            out.truncate(size)
            if kind == 2:
                f.seek(0)
                for pos in range(0, size, 1024 * 1024):
                    out.write(f.read(min(size - pos, 1024 * 1024)))
            elif kind == 3:
                header_offset, = struct.unpack_from('>Q', footer, 16)
                f.seek(header_offset)
                header = f.read(1024)
                if header[:8] != b'cxsparse':
                    raise ValueError('Invalid dynamic VHD header')
                bat_offset, = struct.unpack_from('>Q', header, 16)
                entries, block_size = struct.unpack_from('>II', header, 28)
                bitmap_bytes = ((block_size // 512 + 7) // 8 + 511) & ~511
                f.seek(bat_offset)
                bat = struct.unpack('>' + 'I' * entries, f.read(entries * 4))
                for index, sector in enumerate(bat):
                    if sector == 0xffffffff or index * block_size >= size:
                        continue
                    f.seek(sector * 512)
                    bitmap = f.read(bitmap_bytes)
                    for i in range(block_size // 512):
                        data = f.read(512)
                        if bitmap[i // 8] & (0x80 >> (i % 8)):
                            out.seek(index * block_size + i * 512)
                            out.write(data)
            else:
                raise ValueError(f'Unsupported VHD type {kind}')
    print(f'Created {dst}: {size:,} logical bytes (sparse)')


class FAT:
    """Small FAT12/16 media inspector; also prepares CONFIG/AUTOEXEC on the working copy."""
    def __init__(self, path):
        self.f = path if hasattr(path, 'read') else open(path, 'r+b')
        boot = self.f.read(512)
        self.base = 0
        if not boot[3:11].startswith((b'MSDOS', b'IBM', b'FRDOS')):
            self.base = struct.unpack_from('<I', boot, 454)[0] * 512
            self.f.seek(self.base)
            boot = self.f.read(512)
        self.bps, self.spc, reserved, self.nfats, self.nroot, total, _, self.spf = struct.unpack_from('<HBHBHHBH', boot, 11)
        if self.bps != 512 or not self.spc:
            raise ValueError('Invalid FAT BPB')
        total = total or struct.unpack_from('<I', boot, 32)[0]
        self.fatbase = self.base + reserved * self.bps
        self.rootbase = self.fatbase + self.nfats * self.spf * self.bps
        self.database = self.rootbase + ((self.nroot * 32 + 511) // 512) * 512
        clusters = (total - (self.database - self.base) // 512) // self.spc
        self.bits = 12 if clusters < 4085 else 16
        self.f.seek(self.fatbase)
        self.fat = bytearray(self.f.read(self.spf * 512))

    def entries(self):
        self.f.seek(self.rootbase)
        root = self.f.read(self.nroot * 32)
        for off in range(0, len(root), 32):
            d = root[off:off+32]
            if d[0] == 0:
                break
            if d[0] != 0xe5 and d[11] != 15:
                yield off, d[:11].decode('ascii', 'replace'), struct.unpack_from('<H', d, 26)[0], struct.unpack_from('<I', d, 28)[0]

    def chain(self, cluster):
        seen = set()
        while 2 <= cluster < (0xff8 if self.bits == 12 else 0xfff8):
            if cluster in seen:
                raise ValueError('FAT cycle')
            seen.add(cluster)
            yield cluster
            if self.bits == 16:
                cluster = struct.unpack_from('<H', self.fat, cluster * 2)[0]
            else:
                v = struct.unpack_from('<H', self.fat, cluster * 3 // 2)[0]
                cluster = (v >> 4) if cluster & 1 else (v & 0xfff)

    def read_file(self, cluster, size):
        data = bytearray()
        for c in self.chain(cluster):
            self.f.seek(self.database + (c - 2) * self.spc * 512)
            data.extend(self.f.read(self.spc * 512))
        return bytes(data[:size])

    def replace_existing(self, name, data):
        for off, n, cluster, size in self.entries():
            if n != name:
                continue
            chain = list(self.chain(cluster))
            if len(data) > len(chain) * self.spc * 512:
                raise ValueError('Replacement exceeds allocated clusters')
            remain = data
            for c in chain:
                chunk, remain = remain[:self.spc * 512], remain[self.spc * 512:]
                self.f.seek(self.database + (c - 2) * self.spc * 512)
                self.f.write(chunk.ljust(self.spc * 512, b'\0'))
            self.f.seek(self.rootbase + off + 28)
            self.f.write(struct.pack('<I', len(data)))
            self.f.flush()
            return
        raise ValueError(f'{name} not found')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--inspect', type=Path)
    p.add_argument('--minimal-dos', action='store_true', help='replace CONFIG/AUTOEXEC on the working copy; backups in media/')
    args = p.parse_args()
    if args.inspect:
        fat = FAT(args.inspect)
        for _, name, cluster, size in fat.entries():
            print(name, cluster, size)
            if name in ('CONFIG  SYS', 'AUTOEXECBAT'):
                print(fat.read_file(cluster, size).decode('cp437'))
        return
    src = next(Path('media').rglob('*.vhd'))
    dst = Path('media/dos.raw')
    if not dst.exists():
        convert(src, dst)
    if args.minimal_dos:
        fat = FAT(dst)
        for _, name, cluster, size in list(fat.entries()):
            if name in ('CONFIG  SYS', 'AUTOEXECBAT'):
                backup = Path('media') / (name[:8].strip() + '.' + name[8:].strip() + '.original')
                if not backup.exists():
                    backup.write_bytes(fat.read_file(cluster, size))
        fat.replace_existing('CONFIG  SYS', b'FILES=40\r\nBUFFERS=20\r\nLASTDRIVE=Z\r\n')
        fat.replace_existing('AUTOEXECBAT', b'@ECHO OFF\r\nPATH C:\\DOS\r\nPROMPT $P$G\r\nECHO PCEMU MS-DOS ready\r\n')
    print('Run: make && build/pcemu --disk media/dos.raw')

if __name__ == '__main__':
    main()
