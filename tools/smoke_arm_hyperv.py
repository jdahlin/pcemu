#!/usr/bin/env python3
"""Run original ARM64 Hyper-V/PSCI probes with discovery enabled and disabled.

This tests the ABI; it does not establish use by Windows.
"""
import json
from pathlib import Path
import subprocess
import tempfile
from build_uefi_probe import build
from make_uefi_test_disk import create
from build_arm_platform import build as platform

ROOT = Path(__file__).resolve().parents[1]


def main():
    directory = Path(tempfile.mkdtemp(prefix='hyperv-smoke-', dir=ROOT / 'build'))
    probe = build(directory / 'BOOTAA64.EFI', ROOT / 'tests/uefi/hyperv_probe.S')
    disk = directory / 'probe.raw'
    create(disk, probe)
    platform(1, ROOT / 'build/arm64-fw/cpus1-hyperv', hyperv=True)
    summary = {}
    for mode in ('off', 'basic'):
        stats = directory / f'{mode}.json'
        result = subprocess.run([str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
            '--vars', str(directory / f'{mode}-vars.bin'), '--disk', str(disk),
            '--hyperv', mode, '--hyperv-stats', str(stats), '--timeout-ms', '25000'],
            cwd=ROOT, capture_output=True, timeout=35)
        (directory / f'{mode}-serial.log').write_bytes(result.stdout)
        (directory / f'{mode}-host.log').write_bytes(result.stderr)
        marker = b'PCEMU_HYPERV_PROBE_OK' if mode == 'basic' else b'PCEMU_HYPERV_DISABLED'
        if result.returncode or marker not in result.stdout or b'state=power-off' not in result.stderr:
            raise RuntimeError(f'{mode} guest probe failed: inspect {directory}')
        report = json.loads(stats.read_text())
        if report['mode'] != mode or report['uidQueries'] != 1:
            raise RuntimeError(f'{mode} invalid discovery counters')
        if mode == 'basic':
            names = {r['name']: r for r in report['registers']}
            if (report['guestOsId'] != 0x5043454d55 or report['legacyCalls'] != 1
                    or report['smcccCalls'] != 6 or report['failures'] != 1
                    or names[0x90002]['reads'] != 1 or names[0x90002]['writes'] != 1
                    or names[0x90003]['reads'] != 1
                    or names[0x90015]['reads'] != 1 or names[0x90015]['writes'] != 1
                    or not report['partitionInfoPage'] & 1):
                raise RuntimeError('Guest results and invocation counters disagree')
        elif report['guestOsId'] or report['smcccCalls'] or report['legacyCalls']:
            raise RuntimeError('Disabled mode exposed Hyper-V')
        summary[mode] = report
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'passed': True, 'directory': str(directory), 'modes': summary}, indent=2))


if __name__ == '__main__':
    main()
