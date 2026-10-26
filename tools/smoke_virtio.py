#!/usr/bin/env python3
"""Boot an EFI application and verify virtio FAT writes survive a new VM."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
from build_uefi_probe import build
from make_uefi_test_disk import create

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def run(directory, name, transport, command, expected):
    disk_name = 'data' if command else 'boot'
    args = [str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
            '--disk', str(directory / (disk_name + '.raw')), '--vars', str(directory / (transport + '-' + disk_name + '.vars')),
            '--virtio-transport', transport, '--until-text', expected, '--timeout-ms', '30000']
    if command:
        args += ['--shell-command', command]
    started = time.monotonic()
    result = subprocess.run(args, cwd=ROOT, capture_output=True, timeout=40)
    (directory / (name + '.serial.log')).write_bytes(result.stdout)
    (directory / (name + '.host.log')).write_bytes(result.stderr)
    match = re.search(rb'virtioRequests=(\d+)', result.stderr)
    if result.returncode or expected.encode() not in result.stdout or not match or int(match[1]) == 0:
        raise RuntimeError(f'Virtio {name} failed; inspect {directory}')
    if b'Synchronous Exception' in result.stdout:
        raise RuntimeError(f'Firmware guest exception; inspect {directory}')
    return {'name': name, 'transport': transport, 'requests': int(match[1]),
            'wallSeconds': time.monotonic() - started}


def main():
    directory = Path(tempfile.mkdtemp(prefix='virtio-smoke-', dir=ROOT / 'build'))
    efi = build(directory / 'BOOTAA64.EFI')
    create(directory / 'boot.raw', efi)
    create(directory / 'data.raw')
    parent_hash = {name: digest(directory / name) for name in ('boot.raw', 'data.raw')}
    results = [run(directory, 'pci-boot', 'pci', '', '\r\nPCEMU_EFI_BOOT_OK\r\n')]
    results.append(run(directory, 'mmio-boot', 'mmio', '', '\r\nPCEMU_EFI_BOOT_OK\r\n'))
    results.append(run(directory, 'pci-write', 'pci',
                       'type fs0:\\PROBE.TXT\recho PCEMU_VIRTIO_WRITE_OK > fs0:\\WRITE.TXT\rtype fs0:\\WRITE.TXT',
                       '\r\nPCEMU_VIRTIO_WRITE_OK\r\n'))
    results.append(run(directory, 'pci-restore', 'pci', 'type fs0:\\WRITE.TXT',
                       '\r\nPCEMU_VIRTIO_WRITE_OK\r\n'))
    if any(digest(directory / name) != checksum for name, checksum in parent_hash.items()):
        raise RuntimeError('Immutable disk parent changed')
    delta = directory / 'pci-data.vars.qcow2'
    if delta.stat().st_size > 1024 * 1024:
        raise RuntimeError('Unexpectedly large COW delta for a tiny guest file')
    summary = {'passed': True, 'runs': results, 'parentSHA256': parent_hash,
               'logicalDiskBytesEach': (directory / 'data.raw').stat().st_size,
               'allocatedParentBytes': sum((directory / name).stat().st_blocks * 512 for name in parent_hash),
               'writeDeltaBytes': delta.stat().st_size}
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print(directory)


if __name__ == '__main__':
    main()
