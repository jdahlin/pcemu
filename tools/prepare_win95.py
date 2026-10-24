#!/usr/bin/env python3
"""Stage the user's Windows 95 CD files into a private compressed DOS disk."""
import argparse
import hashlib
import json
import subprocess
import struct
from pathlib import Path
from cow_file import CowFile
from fat_stage import Writer
from prepare_media import convert

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'media/win95-install.pcd')
    parser.add_argument('--dos', type=Path, help='clean DOS raw/PCD parent; defaults to a sparse conversion of the supplied VHD')
    parser.add_argument('--source', type=Path, default=ROOT / 'media/win95-install/win95')
    parser.add_argument('--batch-inf', type=Path, help='optional user-authored MSBATCH.INF')
    args = parser.parse_args()
    destination, source = args.output.resolve(), args.source.resolve()
    parent = args.dos.resolve() if args.dos else destination.with_suffix('.dos.raw')
    if destination.exists():
        parser.error('Preserving existing destination: ' + str(destination))
    if not source.is_dir() or (args.dos and not parent.is_file()):
        parser.error('DOS parent and extracted Windows 95 source directory must exist')
    if not args.dos and parent.exists():
        parser.error('Preserving existing DOS backing: ' + str(parent))
    files = {}
    for path in sorted(source.iterdir()):
        if path.is_file():
            name = path.name.upper()
            Writer.entry(name, 32, 0, 0)  # Validate 8.3 names before creating a disk.
            if name in files:
                parser.error('Duplicate case-insensitive source filename: ' + name)
            files[name] = path.read_bytes()
    if 'SETUP.EXE' not in files:
        parser.error('Windows 95 CD source has no SETUP.EXE')
    if args.batch_inf:
        files['MSBATCH.INF'] = args.batch_inf.read_bytes()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not args.dos:
        originals = list((ROOT / 'media').rglob('*.vhd'))
        if len(originals) != 1:
            parser.error('Expected one supplied DOS VHD; pass --dos with a clean disk instead')
        convert(originals[0], parent)
    subprocess.run(['make', '-s', 'build/disk-tool'], cwd=ROOT, check=True)
    subprocess.run([str(ROOT / 'build/disk-tool'), 'overlay', str(destination), str(parent)], check=True)
    # CowFile edits only this new child. The parent remains immutable and no
    # full-sized raw staging copy is needed. Failed preparation is retained.
    with CowFile(destination) as handle:
        fat = Writer(handle)
        if any(name.strip() in ('WIN95', 'WINDOWS') for _, name, _, _ in fat.entries()):
            raise ValueError('DOS parent already contains Windows or installer files')
        fat.replace_existing('CONFIG  SYS', b'DEVICE=C:\\DOS\\HIMEM.SYS /TESTMEM:OFF\r\n'
                             b'FILES=80\r\nBUFFERS=30\r\nLASTDRIVE=Z\r\n')
        fat.replace_existing('AUTOEXECBAT', b'@ECHO OFF\r\nPATH C:\\DOS\r\nPROMPT $P$G\r\n'
                             b'ECHO PCEMU MS-DOS ready\r\n')
        fat.put_directory('WIN95', files)
        directory = next(entry for entry in fat.entries() if entry[1] == 'WIN95      ')
        data = fat.read_file(directory[2], sum(fat.cluster_bytes for _ in fat.chain(directory[2])))
        entries = {data[n:n+11].decode('ascii'): data[n:n+32]
                   for n in range(0, len(data), 32) if data[n] not in (0, 229)}
        for name, expected in files.items():
            entry = entries[bytes(Writer.entry(name, 32, 0, 0)[:11]).decode('ascii')]
            cluster, size = struct.unpack_from('<HI', entry, 26)
            if fat.read_file(cluster, size) != expected:
                raise ValueError('Staged file readback mismatch: ' + name)
    manifest = {'scope': 'Windows 95 installer source staged on clean DOS; not installation success',
                'disk': str(destination), 'parent': str(parent), 'source': str(source),
                'compressedBytes': destination.stat().st_size,
                'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, data in files.items()}}
    destination.with_suffix(destination.suffix + '.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Staged and verified {len(files)} files in {destination} '
          f'({destination.stat().st_size:,} compressed bytes)')


if __name__ == '__main__':
    main()
