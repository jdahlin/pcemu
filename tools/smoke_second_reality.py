#!/usr/bin/env python3
"""Headless setup smoke for the unmodified, checksum-pinned Second Reality release.

This checks DOS loading/unpacking and setup entry, not demo completion or pixels.
The shared FreeDOS parent is read-only; successful temporary overlays are removed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile
from cow_file import CowFile
from fat_stage import Writer
from freedos_fixture import DEFAULT, put_root, startup

ROOT = Path(__file__).resolve().parents[1]
URL = 'https://files.scene.org/get/parties/1993/assembly93/demo/2nd_real.zip'
ARCHIVE_SHA256 = '9dfdb9c312d736003cc363b151b6f366c481b3bd3bb78233cfff72ae677c5c95'
FILES = {
    'SECOND.EXE': 'a0b637b1c6729aeee8874cb7de701f608fe6ec3028fd72652a962a3a2bd25d8f',
    'REALITY.FC': 'cffb5839d0660f646c2474ff46452543997c3fc9469fa64401556c2d271aa63b',
}


def digest(path):
    """Identify exact input bytes, including the reusable read-only disk parent."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def release(archive):
    """Fetch and verify the original distribution; extract only two explicit files."""
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['curl', '--fail', '--location', '--retry', '2', '--max-time', '120',
                        URL, '-o', str(archive)], check=True)
    if digest(archive) != ARCHIVE_SHA256:
        raise ValueError('Second Reality archive checksum mismatch')
    with zipfile.ZipFile(archive) as source:
        files = {name: source.read(name) for name in FILES}
    for name, data in files.items():
        if hashlib.sha256(data).hexdigest() != FILES[name]:
            raise ValueError('Release file checksum mismatch: ' + name)
    return files


def run(args, backend, files):
    """Require the actual setup screen; preserve bounded failure evidence."""
    work = Path(tempfile.mkdtemp(prefix='second-reality-setup-', dir=ROOT/'build'))
    disk = work/'disk.qcow2'
    parent = args.disk.resolve()
    record = {'scope': 'setup-only', 'backend': backend, 'status': 'failed',
              'archiveSHA256': ARCHIVE_SHA256, 'releaseFiles': FILES,
              'parentSHA256': digest(parent)}
    success = False
    try:
        subprocess.run([str(ROOT/'build/disk-tool'), 'overlay', str(disk), str(parent)], check=True)
        with CowFile(disk) as handle:
            fat = Writer(handle)
            for name, data in files.items():
                put_root(fat, name, data)
            startup(fat, b'DEVICE=C:\\FREEDOS\\BIN\\HIMEMX.EXE\r\nDOS=HIGH\r\nFILES=20\r\nBUFFERS=10\r\n',
                    b'@ECHO OFF\r\nC:\\SECOND.EXE\r\nECHO SECOND REALITY RETURNED\r\n')
            fat.commit()
        runner = work/'runner'
        shutil.copyfile(args.runner.resolve(), runner)
        runner.chmod(0o755)
        rom = work/'vga.bin'
        shutil.copyfile(ROOT/'build/firmware/pcemu-vga.bin', rom)
        record.update(runnerSHA256=digest(runner), romSHA256=digest(rom))
        command = [str(runner), '--headless', '--backend', backend, '--no-network',
                   '--disk', str(disk), '--vgabios', str(rom), '--steps', str(args.steps),
                   '--until-text', 'Demo looping:', '--frame', str(work/'setup.ppm'),
                   '--state-json', str(work/'state.json')]
        with (work/'run.log').open('w') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=args.timeout)
        text = (work/'run.log').read_text()
        if result.returncode or 'Demo looping:' not in text or 'Soundcard:' not in text:
            raise RuntimeError('Setup was not reached; inspect ' + str(work/'run.log'))
        state = json.loads((work/'state.json').read_text())
        record['instructions'] = state['instructions']
        if digest(parent) != record['parentSHA256']:
            raise RuntimeError('Read-only FreeDOS parent changed')
        record['status'] = 'passed'
        success = True
        print('PASS Second Reality setup', backend, work, flush=True)
    except BaseException as error:
        record['error'] = str(error)
        raise
    finally:
        (work/'report.json').write_text(json.dumps(record, indent=2)+'\n')
        if success:
            for name in ['disk.qcow2', 'runner', 'vga.bin']:
                (work/name).unlink(missing_ok=True)
        print('Evidence:', work, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, default=DEFAULT, help='existing FreeDOS validation fixture')
    parser.add_argument('--archive', type=Path, default=ROOT/'build/second-reality/2nd_real.zip')
    parser.add_argument('--runner', type=Path, default=ROOT/'build/pcemu')
    parser.add_argument('--backend', choices=['both', 'interpreter', 'jit'], default='both')
    parser.add_argument('--steps', type=int, default=40000000)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    if not args.disk.exists():
        parser.error('Prepare the shared parent with python3 tools/freedos_fixture.py')
    files = release(args.archive)
    for backend in ['interpreter', 'jit'] if args.backend == 'both' else [args.backend]:
        run(args, backend, files)


if __name__ == '__main__':
    main()
