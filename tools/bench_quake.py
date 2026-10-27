#!/usr/bin/env python3
"""Run an original DOS Quake timedemo headlessly, report FPS, and quit."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path
from cow_file import CowFile
from fat_stage import Writer

ROOT = Path(__file__).resolve().parents[1]


def read_guest_file(fat, path):
    """Read a short FAT16 path offline, without mounting or flattening the disk."""
    entries = list(fat.entries())
    parts = path.upper().split('/')
    for index, part in enumerate(parts):
        name = bytes(Writer.entry(part, 32, 0, 0)[:11]).decode('ascii')
        entry = next((e for e in entries if e[1] == name), None)
        if entry is None:
            raise ValueError('Guest file is missing: ' + path)
        _, _, cluster, size = entry
        if index == len(parts) - 1:
            return fat.read_file(cluster, size)
        directory = fat.read_file(cluster, len(list(fat.chain(cluster))) * fat.cluster_bytes)
        entries = []
        for off in range(0, len(directory), 32):
            if directory[off] == 0:
                break
            if directory[off] == 229 or directory[off + 11] == 15:
                continue
            cluster, size = struct.unpack_from('<HI', directory, off + 26)
            entries.append((off, directory[off:off + 11].decode('ascii'), cluster, size))
    raise ValueError('Empty guest path')


def pak_demos(pak):
    """Find embedded .dem entries in the original PACK directory, checking bounds."""
    if len(pak) < 12:
        raise ValueError('Truncated PAK header')
    magic, offset, size = struct.unpack_from('<4sII', pak)
    if magic != b'PACK' or size % 64 or offset < 12 or offset + size > len(pak):
        raise ValueError('Invalid PAK directory')
    demos = []
    for n in range(offset, offset + size, 64):
        raw, start, length = struct.unpack_from('<56sII', pak, n)
        name = raw.split(b'\0', 1)[0].decode('ascii')
        if start + length > len(pak):
            raise ValueError('PAK entry outside archive: ' + name)
        if re.fullmatch(r'[A-Za-z0-9_]{1,8}\.dem', name, re.IGNORECASE) and length:
            demos.append(name[:-4].lower())
    if not demos:
        raise ValueError('No playable demo found in ID1/PAK0.PAK')
    return sorted(set(demos))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, default=ROOT / 'media/quake.qcow2')
    parser.add_argument('--demo', help='embedded demo name; defaults to the first bundled demo')
    parser.add_argument('--timeout', type=int, default=1800, help='maximum seconds including DOS boot and demo loading')
    parser.add_argument('--keep-disk', action='store_true')
    args = parser.parse_args()
    if args.timeout <= 0 or not args.disk.is_file():
        parser.error('A prepared Quake disk and a positive timeout are required; see docs/quake.md')
    subprocess.run(['make', '-s', 'build/quake-bench', 'build/disk-tool'], cwd=ROOT, check=True)
    work = Path(tempfile.mkdtemp(prefix='quake-benchmark-', dir=ROOT / 'build'))
    disk = work / 'guest.qcow2'
    subprocess.run([str(ROOT / 'build/disk-tool'), 'overlay', str(disk), str(args.disk.resolve())], check=True)
    try:
        with CowFile(disk) as handle:
            fat = Writer(handle)
            demos = pak_demos(read_guest_file(fat, 'ID1/PAK0.PAK'))
            demo = args.demo.lower().removesuffix('.dem') if args.demo else demos[0]
            if demo not in demos:
                raise ValueError('Demo not found; available: ' + ', '.join(demos))
            startup = ('@ECHO OFF\r\nPATH C:\\DOS;C:\\QUAKE\r\nCD \\\r\n'
                       'C:\\QUAKE\\QUAKE -nosound -nocdaudio -nojoy -nomouse -noipx -noudp -condebug '
                       '+vid_mode 0 +_vid_default_mode 0 +vid_wait 0 +viewsize 100 +timedemo ' + demo + '\r\nECHO PCEMU QUAKE BENCHMARK DONE\r\n')
            fat.replace_existing('AUTOEXECBAT', startup.encode('ascii'))
        print(f'Quake timedemo: {demo} (320x200, interpreter, silent, uncapped)', flush=True)
        command = [str(ROOT / 'build/quake-bench'), '--disk', str(disk), '--output', str(work / 'result.json'),
                   '--timeout', str(args.timeout)]
        with (work / 'stdout.log').open('w') as out, (work / 'stderr.log').open('w') as err:
            result = subprocess.run(command, cwd=ROOT, stdout=out, stderr=err, timeout=args.timeout + 30)
        if result.returncode:
            raise RuntimeError('Timedemo failed: ' + (work / 'stderr.log').read_text().strip())
        report = json.loads((work / 'result.json').read_text())
        # Confirm the same result was actually written to disk, not merely
        # observed in a DOS call that could have failed.
        with CowFile(disk) as handle:
            console = read_guest_file(Writer(handle), 'ID1/QCONSOLE.LOG').decode('ascii', errors='replace')
        (work / 'qconsole.log').write_text(console)
        if not re.search(rf'\b{report["frames"]} frames\s+[0-9.]+ seconds\s+{report["fps"]:.1f} fps', console):
            raise RuntimeError('Stopped guest console log did not confirm the FPS result')
        report.update(demo=demo, command=command, resolution=[320, 200], sound=False,
                      executableSha256=hashlib.sha256((ROOT / 'build/quake-bench').read_bytes()).hexdigest())
        (work / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        if not args.keep_disk:
            disk.unlink()
            Path(str(disk) + '.idx').unlink(missing_ok=True)
        print(f'{demo}: {report["frames"]} frames in {report["seconds"]:.1f} seconds — {report["fps"]:.1f} FPS')
        print(f'Results: {work / "result.json"}')
    except Exception:
        print(f'Benchmark failure evidence retained in {work}', flush=True)
        raise


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
