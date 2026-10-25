#!/usr/bin/env python3
"""Stage Windows 3.11 setup in a new compressed child of a clean DOS disk."""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path
from cow_file import CowFile
from fat_stage import Writer
from prepare_media import FAT

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dos', type=Path, default=ROOT / 'media/dos.raw')
    parser.add_argument('--output', type=Path, default=ROOT / 'media/win311-install.pcd')
    parser.add_argument('--answer-file', type=Path, help='optional Windows Setup SHH file staged as C:\\PCEMU\\INSTALL.SHH')
    parser.add_argument('--source', type=Path, help='flat extracted installer directory; otherwise use media/install-files or DISK*.IMG')
    args = parser.parse_args()
    parent, output = args.dos.resolve(), args.output.resolve()
    if output.exists() or args.output.is_symlink():
        parser.error('Preserving existing destination: ' + str(output))
    if not parent.is_file():
        parser.error('Clean DOS parent is missing')
    source = args.source or ROOT / 'media/install-files'
    files = {}
    def add(name, data):
        name = name.upper()
        Writer.entry(name, 32, 0, 0)
        if name in files and files[name] != data:
            raise ValueError('Conflicting installer filename: ' + name)
        files[name] = data
    if source.is_dir():
        for path in sorted(source.iterdir()):
            if path.is_file(): add(path.name, path.read_bytes())
    elif args.source:
        parser.error('Installer source directory is missing')
    else:
        for path in sorted((ROOT / 'media').rglob('DISK*.IMG')):
            with path.open('rb') as handle:
                floppy = FAT(handle)
                for off, name, cluster, size in floppy.entries():
                    handle.seek(floppy.rootbase + off + 11)
                    if not handle.read(1)[0] & 0x18:
                        add(name[:8].strip() + '.' + name[8:].strip(), floppy.read_file(cluster, size))
    if 'SETUP.EXE' not in files or 'SETUP.INF' not in files:
        parser.error('Windows 3.11 SETUP.EXE/SETUP.INF source is missing')
    answer = ('\r\n'.join(args.answer_file.read_text().splitlines()) + '\r\n').encode('cp1252') if args.answer_file else None
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['make', '-s', 'build/disk-tool'], cwd=ROOT, check=True)
    subprocess.run([str(ROOT / 'build/disk-tool'), 'overlay', str(output), str(parent)], check=True)
    # A failed preparation retains only its newly owned child for diagnosis.
    with CowFile(output) as handle:
        fat = Writer(handle)
        if any(e[1].strip() in ('WINDOWS', 'WINSETUP', 'WIN95') for e in fat.entries()):
            raise ValueError('Parent contains an existing Windows installation or installer')
        fat.replace_existing('CONFIG  SYS', b'DEVICE=C:\\DOS\\HIMEM.SYS /TESTMEM:OFF\r\nFILES=80\r\nBUFFERS=30\r\nLASTDRIVE=Z\r\n')
        fat.replace_existing('AUTOEXECBAT', b'@ECHO OFF\r\nPATH C:\\DOS\r\nPROMPT $P$G\r\nECHO PCEMU MS-DOS ready\r\n')
        fat.put_directory('WINSETUP', files)
        if answer is not None:
            fat.put_directory('PCEMU', {'INSTALL.SHH': answer})
        directory = next(e for e in fat.entries() if e[1] == 'WINSETUP   ')
        data = fat.read_file(directory[2], sum(1 for _ in fat.chain(directory[2])) * fat.cluster_bytes)
        entries = {data[n:n+11]: data[n:n+32] for n in range(0, len(data), 32) if data[n] not in (0, 229)}
        for name, expected in files.items():
            entry = entries[bytes(Writer.entry(name, 32, 0, 0)[:11])]
            cluster, size = struct.unpack_from('<HI', entry, 26)
            if fat.read_file(cluster, size) != expected:
                raise ValueError('Installer readback mismatch: ' + name)
        if answer is not None:
            directory = next(e for e in fat.entries() if e[1] == 'PCEMU      ')
            data = fat.read_file(directory[2], fat.cluster_bytes)
            entry = next(data[n:n+32] for n in range(0, len(data), 32) if data[n:n+11] == b'INSTALL SHH')
            cluster, size = struct.unpack_from('<HI', entry, 26)
            if fat.read_file(cluster, size) != answer:
                raise ValueError('Answer file readback mismatch')
    manifest = {'scope': 'Setup source staged and verified; not Windows installation success',
                'disk': str(output), 'parent': str(parent), 'source': str(source),
                'compressedBytes': output.stat().st_size,
                'answerFileSha256': hashlib.sha256(answer).hexdigest() if answer is not None else None,
                'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()} for name, data in files.items()}}
    output.with_suffix(output.suffix + '.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Verified {len(files)} installer files in {output}; {output.stat().st_size:,} compressed bytes')

if __name__ == '__main__':
    main()
