#!/usr/bin/env python3
"""Boot real EDK II, exercise serial input and persist a variable across VMs."""
import json
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def boot(directory, name, command, expected):
    started = time.monotonic()
    with (directory / (name + '.serial.log')).open('wb') as serial, \
            (directory / (name + '.host.log')).open('wb') as host:
        result = subprocess.run([
            str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
            '--vars', str(directory / 'vars.bin'), '--shell-command', command,
            '--until-text', expected, '--timeout-ms', '30000',
        ], cwd=ROOT, stdout=serial, stderr=host, timeout=40)
    output = (directory / (name + '.serial.log')).read_bytes()
    if result.returncode != 0 or expected.encode() not in output:
        raise RuntimeError(f'UEFI {name} failed; inspect {directory}')
    if b'Synchronous Exception' in output:
        raise RuntimeError(f'UEFI guest exception; inspect {directory}')
    return {'name': name, 'wallSeconds': time.monotonic() - started,
            'serialBytes': len(output)}


def main():
    directory = Path(tempfile.mkdtemp(prefix='uefi-smoke-', dir=ROOT / 'build'))
    results = []
    # The expected CRLF-delimited response is guest command output, not the
    # host's injected command or the shell's character-by-character echo.
    results.append(boot(directory, 'write',
                        'set PcemuNv PCEMU_NV_OK\recho PCEMU_WRITE_OK',
                        '\r\nPCEMU_WRITE_OK\r\n'))
    if (directory / 'vars.bin').stat().st_size != 0xc0000:
        raise RuntimeError('Unexpected variable-store size')
    results.append(boot(directory, 'restore', 'echo %PcemuNv%', '\r\nPCEMU_NV_OK\r\n'))
    summary = {'passed': True, 'profile': 'arm64-uefi', 'runs': results,
               'variableBytes': (directory / 'vars.bin').stat().st_size,
               'guestDisksCreated': 0}
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print(directory)


if __name__ == '__main__':
    main()
