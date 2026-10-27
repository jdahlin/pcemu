#!/usr/bin/env python3
"""Stage original DOS Quake shareware on a private disk and capture startup.

This is a compatibility probe, not an assertion that the game renders correctly.
Requires a locally extracted quake106.zip/resource.1 and an existing DOS disk.
"""
import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path

from cow_file import CowFile
from fat_stage import Writer

ROOT = Path(__file__).resolve().parents[1]
PAK_SHA256 = '35a9c55e5e5a284a159ad2a62e0e8def23d829561fe2f54eb402dbc0a9a946af'


def verify_directory(fat, name, files):
    """Read back each staged file before allowing the guest to execute it."""
    directory = next(entry for entry in fat.entries() if entry[1] == name.ljust(11))
    data = fat.read_file(directory[2], len(list(fat.chain(directory[2]))) * fat.cluster_bytes)
    entries = {data[n:n+11]: data[n:n+32] for n in range(0, len(data), 32)
               if data[n] not in (0, 229)}
    for filename, expected in files.items():
        entry = entries[bytes(Writer.entry(filename, 32, 0, 0)[:11])]
        cluster, size = struct.unpack_from('<HI', entry, 26)
        if fat.read_file(cluster, size) != expected:
            raise ValueError('Staged file readback mismatch: ' + filename)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dos', type=Path, required=True, help='stopped, local DOS base disk')
    parser.add_argument('--source', type=Path, required=True, help='extracted directory containing QUAKE.EXE and ID1/PAK0.PAK')
    parser.add_argument('--steps', type=int, default=100_000_000)
    parser.add_argument('--timeout', type=int, default=60)
    parser.add_argument('--no-jit', action='store_true')
    parser.add_argument('--keep-disk', action='store_true', help='retain the private child after capture')
    args = parser.parse_args()
    source, parent = args.source.resolve(), args.dos.resolve()
    if not parent.is_file():
        parser.error('DOS base is missing')
    if args.steps <= 0 or args.timeout <= 0:
        parser.error('Steps and timeout must be positive')
    files = {name: (source / name).read_bytes() for name in ('QUAKE.EXE', 'CWSDPMI.EXE')}
    pak = (source / 'ID1/PAK0.PAK').read_bytes()
    if hashlib.sha256(pak).hexdigest() != PAK_SHA256:
        parser.error('Expected the original Quake 1.06 shareware PAK0.PAK')
    subprocess.run(['make', '-s', 'build/pcemu', 'build/disk-tool'], cwd=ROOT, check=True)
    work = Path(tempfile.mkdtemp(prefix='quake-probe-', dir=ROOT / 'build'))
    disk = work / 'guest.qcow2'
    subprocess.run([str(ROOT / 'build/disk-tool'), 'overlay', str(disk), str(parent)], check=True)
    with CowFile(disk) as handle:
        fat = Writer(handle)
        fat.put_directory('QUAKE', files)
        # -basedir C:\ selects C:\ID1, avoiding any change to the FAT writer's
        # root-directory-only contract.
        fat.put_directory('ID1', {'PAK0.PAK': pak})
        verify_directory(fat, 'QUAKE', files)
        verify_directory(fat, 'ID1', {'PAK0.PAK': pak})
        fat.replace_existing('CONFIG  SYS', b'DEVICE=C:\\DOS\\HIMEM.SYS /TESTMEM:OFF\r\nFILES=80\r\nBUFFERS=30\r\n')
        fat.replace_existing('AUTOEXECBAT', b'@ECHO OFF\r\nPATH C:\\DOS\r\nCD \\QUAKE\r\nQUAKE -basedir C:\\ -nosound -nocdaudio\r\n')
    command = [str(ROOT / 'build/pcemu'), '--headless', '--disk', str(disk),
               '--ram-mb', '32', '--steps', str(args.steps), '--frame', str(work / 'frame.ppm'),
               '--state-json', str(work / 'state.json')]
    if args.no_jit:
        command.append('--no-jit')
    with (work / 'stdout.txt').open('w') as out, (work / 'stderr.txt').open('w') as err:
        try:
            result = subprocess.run(command, cwd=ROOT, stdout=out, stderr=err, timeout=args.timeout)
            status = result.returncode
        except subprocess.TimeoutExpired:
            status = 124
    report = {'scope': 'startup capture only; gameplay is not verified', 'exitCode': status,
              'command': command, 'parent': str(parent),
              'files': {name: hashlib.sha256(data).hexdigest() for name, data in
                        {**files, 'ID1/PAK0.PAK': pak}.items()},
              'stderr': (work / 'stderr.txt').read_text(),
              'diskRetained': args.keep_disk or status != 0}
    if not report['diskRetained']:
        disk.unlink()
        Path(str(disk) + '.idx').unlink(missing_ok=True)
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Startup capture exit={status}: {work}')
    return status


if __name__ == '__main__':
    raise SystemExit(main())
