#!/usr/bin/env python3
"""Dry-run repository storage cleanup; --apply removes verified scratch artifacts.

Preserves snapshots, COW ancestors, current boot disks, original archives, Git
files and symlinks. --obsolete-media also removes named historical debug branches.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
MIB = 1024**2
BOOT_DISKS = {'dos.raw', 'win311.raw', 'win95.raw', 'rtts-win95.img'}


def disk_parents(root):
    parents = set()
    for directory in ('build', 'media'):
        for p in (root/directory).rglob('*'):
            if not p.is_file() or p.suffix not in ('.raw', '.pcd', '.img'):
                continue
            with p.open('rb') as f:
                if f.read(8) != b'PCEMUCOW':
                    continue
                header = f.read(20)
                if len(header) != 20:
                    raise ValueError(f'Truncated COW header: {p}')
                size = struct.unpack_from('<I', header, 16)[0]
                if not 0 < size <= 4096:
                    raise ValueError(f'Invalid COW parent: {p}')
                parent = Path(f.read(size).decode('utf-8'))
                if not parent.is_absolute():
                    parent = p.parent/parent
                if not parent.exists():
                    raise ValueError(f'Missing COW ancestor: {p} -> {parent}')
                parents.add(parent.resolve())
    return parents


def protected_paths(root, tracked):
    protected = disk_parents(root) | {(root/p).resolve() for p in tracked if p}
    protected |= {p.resolve() for d in ('build', 'media') for p in (root/d).rglob('*') if p.is_symlink()}
    return protected


def plan(root, obsolete_media=False, tracked=()):
    root = root.resolve()
    protected = protected_paths(root, tracked)
    remove, compress = [], []
    for directory in ('build', 'media'):
        for p in (root/directory).rglob('*'):
            if p.is_symlink() or not p.is_file() or not p.resolve().is_relative_to(root):
                continue
            relative = p.relative_to(root)
            if any(ancestor in protected for ancestor in (p.resolve(), *p.resolve().parents)) or relative.parts[:2] == ('build', 'snapshots'):
                continue
            size = p.stat().st_size
            reason = None
            if directory == 'build':
                if p.suffix == '.raw' and any(part.startswith(('win311-smoke-', 'reset-smoke-')) for part in relative.parts):
                    reason = 'private smoke-test disk'
                elif p.suffix == '.bin' and size >= MIB:
                    try:
                        state = json.loads(Path(str(p)+'.json').read_text())
                        if 'cr0' in state and 'registers' in state:
                            reason = 'RAM dump with retained CPU-state JSON'
                    except (OSError, ValueError):
                        pass
                    legacy_dump = ('ram' in p.stem or p.stem.startswith(('rtts', 'win95', 'win311', 'rom-', 'bios-')))
                    if reason is None and p.parent == root/'build' and legacy_dump and size in {2*MIB, 4*MIB, 8*MIB, 16*MIB, 32*MIB, 64*MIB}:
                        compress.append((p, 'lossless archive of older RAM diagnostic'))
                elif (p.suffix == '.ppm' and size >= MIB//2) or (p.suffix in ('.log', '.txt') and size >= 8*MIB):
                    compress.append((p, 'lossless diagnostic compression'))
            elif obsolete_media and p.parent == root/'media' and p.name not in BOOT_DISKS:
                if p.suffix == '.raw' and (p.name.startswith(('rtts-', 'win95-')) or
                                          p.name in ('win311-install.raw', 'win311-smoke.raw', 'setup-stage.raw')):
                    reason = 'superseded installation/debug disk; current disks and originals retained'
            if reason:
                remove.append((p, reason))
    return remove, compress


def digest_stream(stream):
    result = hashlib.sha256()
    for block in iter(lambda: stream.read(1024*1024), b''):
        result.update(block)
    return result.hexdigest()


def digest(path):
    with path.open('rb') as f:
        return digest_stream(f)


def verified_duplicates(root):
    result = []
    source = root/'media/office97-win95.pcd'
    retained = root/'build/snapshots/office97-desktop/disk.raw'
    if source.exists() and retained.exists() and source.stat().st_size == retained.stat().st_size and digest(source) == digest(retained):
        result.append((source, 'SHA-256 identical to retained Office desktop disk'))
    archive = root/'media/Microsoft Windows 95B (4.00.1111) (OSR2) [English] (CD).7z'
    member = 'Microsoft Windows 95B (4.00.1111) (OSR2) [English] (CD)/windows95b.iso'
    extracted = root/'media'/member
    if extracted.exists() and archive.exists():
        child = subprocess.Popen(['bsdtar', '-xOf', str(archive), '*/windows95b.iso'], stdout=subprocess.PIPE)
        try:
            archived_hash = digest_stream(child.stdout)
            child.stdout.close()
            if child.wait() != 0:
                raise RuntimeError('Original Windows archive could not be verified')
        finally:
            if child.poll() is None:
                child.terminate(); child.wait()
        if archived_hash == digest(extracted):
            result.append((extracted, 'SHA-256 identical to ISO inside retained original 7z'))
    return result


def require_stopped_guests():
    commands = subprocess.check_output(['ps', '-axo', 'comm='], text=True).splitlines()
    active = [c for c in commands if Path(c.strip()).name == 'disk-tool' or
              Path(c.strip()).name == 'pcemu' or Path(c.strip()).name.startswith('pcemu-')]
    if active:
        raise RuntimeError('Stop running VMs/disk-tool before cleanup: '+', '.join(active))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--obsolete-media', action='store_true')
    args = parser.parse_args()
    os.chdir(ROOT)
    tracked = subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0')
    remove, compress = plan(ROOT, args.obsolete_media, filter(None, tracked))
    if args.obsolete_media:
        remove += verified_duplicates(ROOT)
    protected = protected_paths(ROOT, tracked)
    remove = [(p, why) for p, why in remove if not any(a in protected for a in (p.resolve(), *p.resolve().parents))]
    report = {'apply': args.apply, 'time': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
              'free_before': shutil.disk_usage(ROOT).free, 'removed': [], 'compressed': []}
    print(f'{len(remove)} removable files, {len(compress)} compressible diagnostics', flush=True)
    print(f'Removable reported allocation: {sum(p.stat().st_blocks*512 for p, _ in remove)/1024**3:.2f} GiB (APFS clones can share blocks)', flush=True)
    if args.apply:
        require_stopped_guests()
    for p, why in remove:
        stat = p.stat()
        report['removed'].append({'path': str(p.relative_to(ROOT)), 'reason': why,
                                  'logical_bytes': stat.st_size, 'reported_allocated_bytes': stat.st_blocks*512})
        if args.apply:
            p.unlink()
    for p, why in compress:
        destination = p.with_name(p.name+'.zst')
        if destination.exists():
            continue
        stat = p.stat()
        row = {'path': str(p.relative_to(ROOT)), 'reason': why, 'original_bytes': stat.st_size}
        if args.apply:
            subprocess.run(['zstd', '-q', '-3', '--check', '-o', str(destination), str(p)], check=True)
            subprocess.run(['zstd', '-q', '-t', str(destination)], check=True)
            now = p.stat()
            if now.st_mtime_ns != stat.st_mtime_ns or now.st_size != stat.st_size:
                destination.unlink()
                raise RuntimeError(f'Diagnostic changed during compression; original retained: {p}')
            if destination.stat().st_size >= stat.st_size:
                destination.unlink()
                continue
            row['compressed_bytes'] = destination.stat().st_size
            p.unlink()
        report['compressed'].append(row)
    report['free_after'] = shutil.disk_usage(ROOT).free
    output = ROOT/'build/storage-cleanup'
    output.mkdir(parents=True, exist_ok=True)
    name = 'applied' if args.apply else 'plan'
    latest = output/(name+'.json')
    if args.apply and latest.exists():
        previous = latest.read_bytes()
        archive = output/('history-applied-'+hashlib.sha256(previous).hexdigest()[:12]+'.json')
        if not archive.exists():
            archive.write_bytes(previous)
    latest.write_text(json.dumps(report, indent=2)+'\n')
    print(f'Report: {output/name}.json', flush=True)
    if args.apply:
        print(f'Filesystem free-space increase: {(report["free_after"]-report["free_before"])/1024**3:.2f} GiB', flush=True)


if __name__ == '__main__':
    main()
