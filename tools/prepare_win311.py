#!/usr/bin/env python3
"""Prepare an independent clean DOS disk for the full Windows 3.11 installer."""
from pathlib import Path
from prepare_media import convert,FAT
from fat_stage import Writer

p=Path('media/win311-install.raw')
if p.exists():raise SystemExit('Preserving existing Windows 3.11 installer disk.')
convert(next(Path('media').rglob('*.vhd')),p)
f=Writer(p)
f.replace_existing('CONFIG  SYS',b'DEVICE=C:\\DOS\\HIMEM.SYS /TESTMEM:OFF\r\nFILES=80\r\nBUFFERS=30\r\nLASTDRIVE=Z\r\n')
f.replace_existing('AUTOEXECBAT',b'@ECHO OFF\r\nPATH C:\\DOS\r\nPROMPT $P$G\r\nECHO PCEMU MS-DOS ready\r\n')
files={}
for source in sorted(Path('media').rglob('DISK*.IMG')):
    floppy=FAT(source)
    for _,name,cluster,size in floppy.entries():
        if size:files[name[:8].strip()+'.'+name[8:].strip()]=floppy.read_file(cluster,size)
    floppy.f.close()
if 'SETUP.EXE' not in files:raise ValueError('Windows 3.11 setup source missing')
f.put_directory('WINSETUP',files)
f.f.close()
print('Prepared full installer source:',len(files),'files')
