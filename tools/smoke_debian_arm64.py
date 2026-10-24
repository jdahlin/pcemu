#!/usr/bin/env python3
"""Boot the real Debian ARM64 installer through UEFI and a private virtio COW."""
import json
from pathlib import Path
import subprocess
import tempfile
from prepare_debian_arm64 import ROOT, fetch, create


def main():
    cache = ROOT / 'build/debian-arm64'
    fetch(cache)
    directory = Path(tempfile.mkdtemp(prefix='debian-boot-', dir=ROOT / 'build'))
    create(directory / 'netboot.raw', cache)
    args = [str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
            '--disk', str(directory / 'netboot.raw'), '--vars', str(directory / 'vars.bin'),
            '--until-text', 'Select a language', '--timeout-ms', '60000', '--cpu-state']
    result = subprocess.run(args, cwd=ROOT, capture_output=True, timeout=75)
    (directory / 'serial.log').write_bytes(result.stdout)
    (directory / 'host.log').write_bytes(result.stderr)
    markers = [b'EFI stub: Booting Linux Kernel', b'Linux version ', b'GICv3:',
               b'legacy console [ttyAMA0] enabled', b'Select a language']
    passed = result.returncode == 0 and all(marker in result.stdout for marker in markers)
    passed &= b'Kernel panic' not in result.stdout and b'Synchronous Exception' not in result.stdout
    summary = {'passed': bool(passed), 'milestone': 'Debian installer language selection',
               'installedSystem': False, 'sshVerified': False, 'command': args}
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print(directory)
    if not passed:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
