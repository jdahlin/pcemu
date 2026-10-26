#!/usr/bin/env python3
"""Verify a guest-requested reboot and persistent EFI state in one runner."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
from build_uefi_probe import build
from make_uefi_test_disk import create

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def main():
    directory = Path(tempfile.mkdtemp(prefix='reboot-smoke-', dir=ROOT / 'build'))
    probe = build(directory / 'BOOTAA64.EFI', ROOT / 'tests/uefi/reboot_probe.S')
    disk = directory / 'probe.raw'
    create(disk, probe)
    original = digest(disk)
    started = time.monotonic()
    with (directory / 'serial.log').open('wb') as serial, (directory / 'host.log').open('wb') as host:
        result = subprocess.run([str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
            '--vars', str(directory / 'vars.bin'), '--disk', str(disk),
            '--timeout-ms', '25000'],
            cwd=ROOT, stdout=serial, stderr=host, timeout=35)
    output = (directory / 'serial.log').read_bytes()
    if result.returncode or b'PCEMU_REBOOT_OK' not in output or b'Synchronous Exception' in output:
        raise RuntimeError(f'Guest reboot probe failed: inspect {directory}')
    host_output = (directory / 'host.log').read_text()
    if ('state=power-off' not in host_output or
            host_output.count('UEFI restarting after guest reset: reboot=1') != 1 or
            host_output.count('state=reset-requested') != 1):
        raise RuntimeError(f'PSCI SYSTEM_OFF did not stop VM: inspect {directory}')
    if digest(disk) != original:
        raise RuntimeError('Immutable probe disk changed')
    summary = {'passed': True, 'platform': 'arm-virt-v3',
               'wallSeconds': time.monotonic() - started,
               'validated': ['EFI ResetSystem', 'PSCI SYSTEM_RESET', 'UEFI variable persistence',
                             'platform recreation', 'PSCI SYSTEM_OFF'],
               'diskAllocatedBytes': disk.stat().st_blocks * 512,
               'overlayBytes': (directory / 'vars.bin.qcow2').stat().st_size}
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print(directory)


if __name__ == '__main__':
    main()
