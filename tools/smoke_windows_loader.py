#!/usr/bin/env python3
"""Optional proprietary-media check: Windows Boot Manager reaches its BCD error.

Use the loader-only disk from inspect_validation_os.py --make-loader-probe.
This deliberately checks an intermediate milestone, not a Windows OS boot.
The input disk is reused read-only with a tiny private COW; no media is downloaded.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
from smoke_arm_acpi import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, default=ROOT / 'build/validation-boot/loader-probe.raw')
    args = parser.parse_args()
    disk = args.disk.resolve(strict=True)
    original = digest(disk)
    directory = Path(tempfile.mkdtemp(prefix='windows-loader-smoke-', dir=ROOT / 'build'))
    started = time.monotonic()
    with (directory / 'serial.log').open('wb') as serial, (directory / 'host.log').open('wb') as host:
        result = subprocess.run([str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
            '--vars', str(directory / 'vars.bin'), '--disk', str(disk), '--cpu-state',
            '--until-text', '0xc000000f', '--timeout-ms', '15000'],
            cwd=ROOT, stdout=serial, stderr=host, timeout=25)
    output = (directory / 'serial.log').read_text()
    clean = re.sub(r'\x1b\[[0-9;=?]*[A-Za-z]', '', output)
    if result.returncode or any(text not in clean for text in (
            'Windows Boot Manager', '\\EFI\\Microsoft\\Boot\\BCD', '0xc000000f')):
        raise RuntimeError(f'Windows loader milestone failed: inspect {directory}')
    if 'Synchronous Exception' in clean:
        raise RuntimeError('Windows loader raised a CPU exception')
    if digest(disk) != original:
        raise RuntimeError('Immutable Windows probe disk changed')
    summary = {'passed': True, 'milestone': 'Windows Boot Manager reports missing BCD',
               'windowsOsBooted': False, 'wallSeconds': time.monotonic() - started,
               'immutableParent': str(disk),
               'overlayBytes': (directory / 'vars.bin.pcd').stat().st_size}
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print(directory)


if __name__ == '__main__':
    main()
