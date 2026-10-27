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


def demo_from_pak(pak, name):
    """Extract a selected directory entry after pak_demos has validated bounds."""
    _, offset, size = struct.unpack_from('<4sII', pak)
    for n in range(offset, offset + size, 64):
        raw, start, length = struct.unpack_from('<56sII', pak, n)
        if raw.split(b'\0', 1)[0].decode('ascii').lower() == name.lower() + '.dem':
            return pak[start:start + length]
    raise ValueError('Demo is missing from PAK: ' + name)


def shorten_demo(demo, frames):
    """Keep stock shareware signon/warmup, then N timed packets and disconnect.

    The original demo1/2/3 consume five records during the initial unmeasured
    frame. Subsequent timedemo frames consume one record each. Preserve complete
    messages and view angles; never splice network payloads. The caller must
    verify Quake's reported count, since arbitrary third-party recordings may
    have a different signon sequence.
    https://github.com/id-Software/Quake/blob/master/WinQuake/cl_demo.c
    """
    header_end = demo.find(b'\n', 0, 16) + 1
    if not header_end or not re.fullmatch(rb'-?[0-9]+\n', demo[:header_end]):
        raise ValueError('Invalid demo CD-track header')
    records = []
    at = header_end
    while at < len(demo):
        if at + 16 > len(demo):
            raise ValueError('Truncated demo record header')
        length = struct.unpack_from('<i', demo, at)[0]
        if length <= 0 or length > 8000 or at + 16 + length > len(demo):
            raise ValueError('Invalid or truncated demo message')
        records.append(demo[at:at + 16 + length])
        at += 16 + length
    if not records or records[-1][16:] != b'\x02':
        raise ValueError('Expected a stock demo ending with svc_disconnect')
    available = len(records) - 6
    if frames <= 0 or frames > available:
        raise ValueError(f'Frame count must be between 1 and {available} for this demo')
    kept = records[:5 + frames]
    # svc_disconnect=2 is a complete one-byte network message. Keep the last
    # retained view angles rather than those from the original far-away ending.
    disconnect = struct.pack('<i', 1) + kept[-1][4:16] + b'\x02'
    return demo[:header_end] + b''.join(kept) + disconnect


def put_id1_file(fat, filename, data):
    """Add an owned 8.3 file to existing ID1 on the private child; verify readback."""
    directory = next((e for e in fat.entries() if e[1] == 'ID1        '), None)
    if directory is None:
        raise ValueError('ID1 directory is missing')
    chain = list(fat.chain(directory[2]))
    contents = fat.read_file(directory[2], len(chain) * fat.cluster_bytes)
    name = bytes(Writer.entry(filename, 32, 0, 0)[:11])
    if any(contents[n:n + 11] == name for n in range(0, len(contents), 32)):
        raise ValueError('Preserving existing ID1/' + filename)
    offset = next((n for n in range(0, len(contents), 32) if contents[n] in (0, 229)), None)
    if offset is None:
        raise ValueError('ID1 directory has no free slot for ' + filename)
    cluster = fat.allocate(data)
    entry = fat.entry(filename, 32, cluster, len(data))
    directory_cluster = chain[offset // fat.cluster_bytes]
    fat.f.seek(fat.database + (directory_cluster - 2) * fat.cluster_bytes + offset % fat.cluster_bytes)
    fat.f.write(entry)
    fat.commit()
    if read_guest_file(fat, 'ID1/' + filename) != data:
        raise ValueError('Guest file readback mismatch: ' + filename)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, default=ROOT / 'media/quake.qcow2')
    parser.add_argument('--demo', help='embedded demo name; defaults to the first bundled demo')
    parser.add_argument('--frames', type=int, help='create and run a stock-demo prefix with this many timed frames')
    parser.add_argument('--timeout', type=int, default=1800, help='maximum seconds including DOS boot and demo loading')
    parser.add_argument('--keep-disk', action='store_true')
    args = parser.parse_args()
    if args.timeout <= 0 or (args.frames is not None and args.frames <= 0) or not args.disk.is_file():
        parser.error('A prepared Quake disk, positive timeout and positive frame count are required; see docs/quake.md')
    subprocess.run(['make', '-s', 'build/quake-bench', 'build/disk-tool'], cwd=ROOT, check=True)
    work = Path(tempfile.mkdtemp(prefix='quake-benchmark-', dir=ROOT / 'build'))
    disk = work / 'guest.qcow2'
    subprocess.run([str(ROOT / 'build/disk-tool'), 'overlay', str(disk), str(args.disk.resolve())], check=True)
    try:
        with CowFile(disk) as handle:
            fat = Writer(handle)
            pak = read_guest_file(fat, 'ID1/PAK0.PAK')
            demos = pak_demos(pak)
            demo = args.demo.lower().removesuffix('.dem') if args.demo else demos[0]
            if demo not in demos:
                raise ValueError('Demo not found; available: ' + ', '.join(demos))
            playback_demo = demo
            if args.frames is not None:
                short = shorten_demo(demo_from_pak(pak, demo), args.frames)
                (work / f'{demo}-{args.frames}.dem').write_bytes(short)
                put_id1_file(fat, 'PCBENCH.DEM', short)
                playback_demo = 'pcbench'
            config = ('vid_mode 0\n_vid_default_mode 0\nvid_wait 0\nviewsize 100\n'
                      'timedemo ' + playback_demo + '\n').encode('ascii')
            put_id1_file(fat, 'PCBENCH.CFG', config)
            (work / 'pcbench.cfg').write_bytes(config)
            # DOS EXEC tails are limited to 127 bytes. Put the Quake console
            # settings in a guest config rather than silently truncating them.
            arguments = '-nosound -nocdaudio -nojoy -nomouse -noipx -noudp -condebug +exec pcbench.cfg'
            if len(arguments) > 126:
                raise ValueError('Quake arguments exceed the DOS command-tail limit')
            startup = ('@ECHO OFF\r\nPATH C:\\DOS;C:\\QUAKE\r\nCD \\\r\n'
                       'C:\\QUAKE\\QUAKE ' + arguments + '\r\nECHO PCEMU QUAKE BENCHMARK DONE\r\n')
            fat.replace_existing('AUTOEXECBAT', startup.encode('ascii'))
        label = f'{demo}/{args.frames} frames' if args.frames is not None else demo
        print(f'Quake timedemo: {label} (320x200, interpreter, silent, uncapped)', flush=True)
        command = [str(ROOT / 'build/quake-bench'), '--disk', str(disk), '--output', str(work / 'result.json'),
                   '--timeout', str(args.timeout)]
        with (work / 'stdout.log').open('w') as out, (work / 'stderr.log').open('w') as err:
            result = subprocess.run(command, cwd=ROOT, stdout=out, stderr=err, timeout=args.timeout + 30)
        if result.returncode:
            raise RuntimeError('Timedemo failed: ' + (work / 'stderr.log').read_text().strip())
        report = json.loads((work / 'result.json').read_text())
        if args.frames is not None and report['frames'] != args.frames:
            raise RuntimeError(f'Short demo reported {report["frames"]} frames; expected {args.frames}')
        # Confirm the same result was actually written to disk, not merely
        # observed in a DOS call that could have failed.
        with CowFile(disk) as handle:
            console = read_guest_file(Writer(handle), 'ID1/QCONSOLE.LOG').decode('ascii', errors='replace')
        (work / 'qconsole.log').write_text(console)
        if not re.search(rf'\b{report["frames"]} frames\s+[0-9.]+ seconds\s+{report["fps"]:.1f} fps', console):
            raise RuntimeError('Stopped guest console log did not confirm the FPS result')
        report.update(demo=demo, requestedFrames=args.frames, playbackDemo=playback_demo, command=command, resolution=[320, 200], sound=False,
                      executableSha256=hashlib.sha256((ROOT / 'build/quake-bench').read_bytes()).hexdigest())
        if args.frames is not None:
            report['demoFile'] = f'{demo}-{args.frames}.dem'
            report['demoSha256'] = hashlib.sha256(short).hexdigest()
        (work / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        if not args.keep_disk:
            disk.unlink()
            Path(str(disk) + '.idx').unlink(missing_ok=True)
        print(f'{label}: {report["frames"]} frames in {report["seconds"]:.1f} seconds — {report["fps"]:.1f} FPS')
        print(f'Results: {work / "result.json"}')
    except Exception:
        print(f'Benchmark failure evidence retained in {work}', flush=True)
        raise


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
