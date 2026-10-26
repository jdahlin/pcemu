#!/usr/bin/env python3
"""Headless EFI HDA playback/DMA smoke with a small sparse probe disk.

SDL's dummy audio driver exercises host output without opening a window or
playing sound. All temporary disk/variable files are removed after success.
"""
import os
import re
from pathlib import Path
import subprocess
import tempfile
from build_uefi_probe import build
from make_uefi_test_disk import create

ROOT = Path(__file__).resolve().parents[1]


def main():
    directory = Path(tempfile.mkdtemp(prefix='hda-smoke-', dir=ROOT / 'build'))
    probe = build(directory / 'BOOTAA64.EFI', ROOT / 'tests/uefi/hda_probe.S')
    disk = directory / 'probe.raw'
    create(disk, probe)
    environment = {**os.environ, 'SDL_AUDIODRIVER': 'dummy'}
    with (directory / 'serial.log').open('wb') as serial, (directory / 'host.log').open('wb') as host:
        result = subprocess.run([str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
            '--vars', str(directory / 'vars.bin'), '--disk', str(disk), '--audio', 'ich9',
            '--audio-backend', 'sdl', '--timeout-ms', '15000', '--cpu-state'],
            cwd=ROOT, env=environment, stdout=serial, stderr=host, timeout=25)
    output = (directory / 'serial.log').read_bytes()
    host_output = (directory / 'host.log').read_text()
    playback = re.search(r'hdaOutputFrames=(\d+)', host_output)
    if (result.returncode or not playback or int(playback[1]) == 0
            or b'PCEMU_HDA_OK' not in output or 'state=power-off' not in host_output):
        raise RuntimeError(f'HDA EFI probe failed: inspect {directory}')
    for path in (disk, directory / 'vars.bin', directory / 'vars.bin.qcow2'):
        path.unlink(missing_ok=True)
    print(f'HDA PCI/codec/ring/playback/SDL probe passed: {directory}')


if __name__ == '__main__':
    main()
