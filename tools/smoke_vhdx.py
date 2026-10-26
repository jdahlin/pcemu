#!/usr/bin/env python3
"""UEFI writes to a VHDX directly, then a new VM reads the persisted file."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from make_uefi_test_disk import create as raw_disk
from make_vhdx_test_disk import create as vhdx_disk

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk-controller', choices=('virtio', 'nvme'), default='virtio')
    args = parser.parse_args()
    directory = Path(tempfile.mkdtemp(prefix=args.disk_controller + '-vhdx-smoke-', dir=ROOT / 'build'))
    raw = directory / 'fixture.raw'
    disk = directory / 'disk.vhdx'
    raw_disk(raw)
    vhdx_disk(raw, disk)
    raw.unlink()  # Only our generated intermediate fixture, not user media.
    expected = '\r\nPCEMU_VHDX_WRITE_OK\r\n'
    for name, command in [('write', 'fs0:\recho PCEMU_VHDX_WRITE_OK > VHDX.TXT\rtype VHDX.TXT'),
                          ('restore', 'fs0:\rtype VHDX.TXT')]:
        with (directory / (name + '.serial.log')).open('wb') as serial, \
                (directory / (name + '.host.log')).open('wb') as host:
            result = subprocess.run([str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
                '--disk-controller', args.disk_controller, '--vars', str(directory / 'vars.bin'), '--disk', str(disk),
                '--shell-command', command, '--until-text', expected, '--timeout-ms', '20000'],
                cwd=ROOT, stdout=serial, stderr=host, timeout=30)
        if result.returncode or expected.encode() not in (directory / (name + '.serial.log')).read_bytes():
            raise RuntimeError(f'Direct VHDX {name} failed; inspect {directory}')
        host_log = (directory / (name + '.host.log')).read_text()
        if f'disk={disk}' not in host_log or list(directory.glob('*.qcow2')):
            raise RuntimeError('VHDX was not attached directly')
    summary = {'passed': True, 'diskController': args.disk_controller, 'directReadWrite': True, 'cowFilesCreated': 0,
               'logicalDiskBytes': 64 * 1024 * 1024, 'containerBytes': disk.stat().st_size,
               'allocatedBytes': disk.stat().st_blocks * 512}
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print(directory)


if __name__ == '__main__':
    main()
