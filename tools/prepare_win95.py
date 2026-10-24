#!/usr/bin/env python3
"""Prepare an independent DOS disk containing the user's Windows 95 install CD files."""
from pathlib import Path
from prepare_media import convert
from fat_stage import Writer

def main():
    destination=Path('media/win95-install.raw')
    if destination.exists():
        raise SystemExit('Working installer disk exists; preserving it.')
    convert(next(Path('media').rglob('*.vhd')),destination)
    fat=Writer(destination)
    fat.replace_existing('CONFIG  SYS',b'DEVICE=C:\\DOS\\HIMEM.SYS /TESTMEM:OFF\r\nFILES=80\r\nBUFFERS=30\r\nLASTDRIVE=Z\r\n')
    fat.replace_existing('AUTOEXECBAT',b'@ECHO OFF\r\nPATH C:\\DOS\r\nPROMPT $P$G\r\nECHO PCEMU MS-DOS ready\r\n')
    source=Path('media/win95-install/win95')
    files={p.name.upper():p.read_bytes() for p in sorted(source.iterdir()) if p.is_file()}
    if 'SETUP.EXE' not in files:
        raise ValueError('Windows 95 CD source not extracted')
    fat.put_directory('WIN95',files)
    fat.f.close()
    print(f'Staged {len(files)} Windows 95 installer files in {destination}')

if __name__=='__main__':main()
