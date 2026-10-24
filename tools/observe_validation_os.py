#!/usr/bin/env python3
"""Capture a bounded boot of the original ARM64 Validation OS VHDX.

The disk is writable: use a private downloaded instance. Serial output, CPU state
and GOP pixels are evidence for manual diagnosis, not proof of Windows OS boot.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path,
                        default=ROOT / 'media/validation-os-arm64-vhdx/ValidationOS.vhdx')
    parser.add_argument('--disk-controller', choices=('virtio', 'nvme'), default='virtio')
    parser.add_argument('--timeout-ms', type=int, default=60000)
    args = parser.parse_args()
    if not 1 <= args.timeout_ms <= 60000:
        parser.error('--timeout-ms must be between 1 and 60000')
    disk = args.disk.resolve(strict=True)
    directory = Path(tempfile.mkdtemp(prefix='validation-os-boot-', dir=ROOT / 'build'))
    started = time.monotonic()
    with (directory / 'serial.log').open('wb') as serial, (directory / 'host.log').open('wb') as host:
        result = subprocess.run([
            str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
            '--disk', str(disk), '--disk-controller', args.disk_controller, '--vars', str(directory / 'vars.bin'),
            '--cpu-state', '--framebuffer-ppm', str(directory / 'console.ppm'),
            '--timeout-ms', str(args.timeout_ms),
        ], cwd=ROOT, stdout=serial, stderr=host, timeout=args.timeout_ms / 1000 + 15)
    summary = {'disk': str(disk), 'diskController': args.disk_controller, 'diskWritable': True, 'returncode': result.returncode,
               'wallSeconds': time.monotonic() - started,
               'windowsOsBootVerified': False,
               'note': 'Inspect serial.log, host.log and console.ppm; exit 0 only means the observation ended.'}
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print(directory)
    if result.returncode:
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
