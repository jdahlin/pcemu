#!/usr/bin/env python3
"""Flatten stopped legacy disks into verified, zstd-compressed standard QCOW2.

All conversions finish before --remove-originals removes anything. Ancestors
must be listed as well if they are to be retired. Unlisted COW dependents stop
removal. The converter compares every guest-visible byte and runs qemu-img check.
"""
import argparse
import fcntl
import json
from pathlib import Path
import struct
import subprocess
from contextlib import ExitStack
from convert_qcow2 import convert

ROOT = Path(__file__).resolve().parents[1]

def parent_of(path):
    with path.open('rb') as f:
        header = f.read(28)
        if header[:4] == b'QFI\xfb':
            offset = struct.unpack_from('>Q',header,8)[0]
            length = struct.unpack_from('>I',header,16)[0]
            if not offset: return None
            if not 0 < length <= 1023: raise ValueError('Invalid QCOW2 backing path')
            f.seek(offset)
            parent = Path(f.read(length).decode())
            return (parent if parent.is_absolute() else path.parent/parent).resolve()
        if header[:8] != b'PCEMUCOW':
            return None
        length = struct.unpack_from('<I', header, 24)[0]
        if not 0 < length <= 4096:
            raise ValueError('Invalid legacy backing path: '+str(path))
        parent = Path(f.read(length).decode())
        return (parent if parent.is_absolute() else path.parent/parent).resolve()

def migrate(sources, remove=False):
    sources = [p.resolve() for p in sources]
    if len(set(sources)) != len(sources):
        raise ValueError('Duplicate source')
    destinations = [p.with_suffix('.qcow2') for p in sources]
    if any(s == d for s,d in zip(sources,destinations)):
        raise ValueError('Source already has .qcow2 suffix; convert to an explicit new destination instead')
    for destination in destinations:
        if destination.exists(): raise FileExistsError(destination)
    # Hold shared locks on sources and every backing file throughout all passes.
    chain = set(sources)
    for source in sources:
        current = source
        seen = set()
        while (parent := parent_of(current)) is not None:
            if parent in seen: raise ValueError('Backing cycle')
            seen.add(parent); chain.add(parent); current = parent
    with ExitStack() as stack:
        for p in sorted(chain):
            lock = stack.enter_context(Path(str(p)+'.lock').open('a+b'))
            fcntl.flock(lock,fcntl.LOCK_SH|fcntl.LOCK_NB)
            if subprocess.run(['lsof','-t',str(p)],capture_output=True).stdout.strip():
                raise RuntimeError('Disk is in use: '+str(p))
        if remove:
            for folder in ('media','build'):
                for p in (ROOT/folder).rglob('*'):
                    if not p.is_file() or p.is_symlink() or p.suffix not in ('.pcd','.raw','.img','.qcow2'):
                        continue
                    parent = parent_of(p)
                    if parent in sources and p.resolve() not in sources:
                        raise RuntimeError('Unlisted dependent must be converted first: '+str(p))
        results = []
        for source,destination in zip(sources,destinations):
            size = int(subprocess.check_output([str(ROOT/'build/disk-tool'),'size',str(source)]))
            convert(source,destination,size)
            results.append({'source':str(source),'destination':str(destination),
                            'sourceBytes':source.stat().st_size,'qcow2Bytes':destination.stat().st_size,
                            'guestBytes':size,'byteComparisonPassed':True,'qemuCheckPassed':True})
        if remove:
            for source in sources:
                source.unlink()
                Path(str(source)+'.idx').unlink(missing_ok=True)
                # Keep the advisory lock inode until all held locks close.
        return results

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources',nargs='+',type=Path)
    parser.add_argument('--remove-originals',action='store_true')
    parser.add_argument('--report',required=True,type=Path)
    args = parser.parse_args()
    result = migrate(args.sources,args.remove_originals)
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(result,indent=2)+'\n')
