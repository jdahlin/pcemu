#!/usr/bin/env python3
"""Write a DOS batch script; Microsoft EXPAND running inside pcemu installs apps."""
import struct
from pathlib import Path
from prepare_media import FAT

APPS = ['PROGMAN', 'WINFILE', 'TASKMAN', 'NOTEPAD', 'CALC', 'CONTROL', 'PBRUSH', 'CLOCK', 'WRITE', 'SOL', 'WINMINE', 'RECORDER', 'CARDFILE', 'CLIPBRD']
LIBRARIES = ['SHELL', 'COMMDLG', 'TOOLHELP', 'MMSYSTEM', 'OLECLI', 'OLESVR', 'CARDS', 'LZEXPAND', 'VER', 'WIN87EM']

def main():
    sources = set()
    for image in Path('media').rglob('DISK*.IMG'):
        floppy = FAT(image)
        sources.update(n[:8].strip() + '.' + n[8:].strip() for _, n, _, size in floppy.entries() if size)
        floppy.f.close()
    commands = ['@ECHO OFF', 'C:', 'CD \\WINDOWS']
    for names, ext, sub in [(APPS, 'EXE', ''), (LIBRARIES, 'DLL', 'SYSTEM\\'), (['WSWAP', 'DSWAP'], 'EXE', 'SYSTEM\\'), (['MAIN'], 'CPL', 'SYSTEM\\')]:
        for n in names:
            src = n + '.' + ext[:2] + '_'
            if src not in sources:
                continue
            dst = sub + n + '.' + ext
            commands.append(f'IF NOT EXIST {dst} C:\\WINSETUP\\EXPAND C:\\WINSETUP\\{src} {dst}')
    commands.extend(['ECHO Applications installed.', 'ECHO Launch with WIN /S', ''])
    payload = '\r\n'.join(commands).encode('ascii')
    fat = FAT(Path('media/dos.raw'))
    existing = next((e for e in fat.entries() if e[1] == 'INSTALL BAT'), None)
    if existing:
        fat.replace_existing('INSTALL BAT', payload)
    else:
        free = next(c for c in range(2, len(fat.fat)//2) if struct.unpack_from('<H', fat.fat, c*2)[0] == 0)
        if len(payload) > fat.spc*512:
            raise ValueError('Batch exceeds one cluster')
        struct.pack_into('<H', fat.fat, free*2, 0xffff)
        fat.f.seek(fat.database + (free-2)*fat.spc*512)
        fat.f.write(payload.ljust(fat.spc*512, b'\0'))
        for i in range(fat.nfats):
            fat.f.seek(fat.fatbase+i*fat.spf*512)
            fat.f.write(fat.fat)
        fat.f.seek(fat.rootbase)
        root = fat.f.read(fat.nroot*32)
        off = next(o for o in range(0,len(root),32) if root[o] in (0,229))
        entry = bytearray(32);entry[:11] = b'INSTALL BAT';entry[11] = 0x20
        struct.pack_into('<H', entry, 26, free);struct.pack_into('<I', entry, 28, len(payload))
        fat.f.seek(fat.rootbase+off);fat.f.write(entry)
    fat.f.close()
    print('Wrote C:\\INSTALL.BAT. Run it in DOS inside pcemu.')

if __name__ == '__main__':
    main()
