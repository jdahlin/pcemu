#!/usr/bin/env python3
"""Boot our EFI probe and validate firmware-published ACPI and PSCI in the guest."""
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
    directory = Path(tempfile.mkdtemp(prefix='acpi-smoke-', dir=ROOT / 'build'))
    probe = build(directory / 'BOOTAA64.EFI', ROOT / 'tests/uefi/acpi_probe.S')
    disk = directory / 'probe.raw'
    create(disk, probe)
    original = digest(disk)
    started = time.monotonic()
    with (directory / 'serial.log').open('wb') as serial, (directory / 'host.log').open('wb') as host:
        result = subprocess.run([str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
            '--vars', str(directory / 'vars.bin'), '--disk', str(disk),
            '--timeout-ms', '15000'],
            cwd=ROOT, stdout=serial, stderr=host, timeout=25)
    output = (directory / 'serial.log').read_bytes()
    if result.returncode or b'PCEMU_ACPI_OK' not in output or b'Synchronous Exception' in output:
        raise RuntimeError(f'Guest ACPI/PSCI probe failed: inspect {directory}')
    host_output = (directory / 'host.log').read_text()
    if 'state=power-off' not in host_output:
        raise RuntimeError(f'PSCI SYSTEM_OFF did not stop VM: inspect {directory}')
    if digest(disk) != original:
        raise RuntimeError('Immutable probe disk changed')
    summary = {'passed': True, 'platform': 'arm-virt-v3',
               'wallSeconds': time.monotonic() - started,
               'validated': ['RSDP', 'XSDT', 'FADT', 'DSDT', 'MADT', 'GTDT', 'MCFG', 'SPCR', 'DBG2',
                             'PSCI_VERSION', 'PSCI_CPU_ON', 'PSCI_AFFINITY_INFO', 'PSCI_SYSTEM_OFF',
                             'PMU feature', 'PMCCNTR'],
               'diskAllocatedBytes': disk.stat().st_blocks * 512,
               'overlayBytes': (directory / 'vars.bin.pcd').stat().st_size}
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print(directory)


if __name__ == '__main__':
    main()
