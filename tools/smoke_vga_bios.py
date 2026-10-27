#!/usr/bin/env python3
"""Headless standalone VGA ROM boot probe; optional independent QEMU run."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run_probe(runner, rom, qemu=None):
    """Boot the same 512-byte read-only probe without creating an OS fixture."""
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vga-bios-smoke-', dir=ROOT / 'build') as folder:
        disk = Path(folder) / 'probe.img'
        subprocess.run(['nasm', '-Wall', '-Werror', '-w-reloc-abs-word', '-f', 'bin',
                        str(ROOT / 'tests/vga-bios-boot.asm'), '-o', str(disk)], check=True)
        for backend in ('interpreter', 'jit'):
            result = subprocess.run([
                str(runner), '--headless', '--backend', backend, '--no-network',
                '--vgabios', str(rom), '--execute-vgabios', '--floppy', str(disk),
                '--boot-floppy', '--steps', '2000000', '--until-text', 'PCEMU-VGA-PASS',
            ], capture_output=True, text=True, timeout=30)
            if result.returncode or 'PCEMU-VGA-PASS' not in result.stdout or 'PCEMU-VGA-FAIL' in result.stdout:
                raise RuntimeError(f'{backend} boot failed:\n{result.stdout}\n{result.stderr}')
            print(f'PCEmu {backend}: guest boot probe passed')
        if qemu:
            result = subprocess.run([
                qemu, '-machine', 'pc', '-accel', 'tcg', '-m', '16', '-vga', 'none',
                '-device', f'VGA,romfile={rom}',
                '-drive', f'file={disk},format=raw,if=floppy,readonly=on', '-boot', 'a',
                '-display', 'none', '-serial', 'none', '-monitor', 'none', '-no-reboot',
                '-debugcon', 'stdio', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
            ], capture_output=True, text=True, timeout=30)
            if result.returncode != 33 or result.stdout != 'PCEMU-VGA-PASS':
                raise RuntimeError(f'QEMU boot failed ({result.returncode}):\n{result.stdout}\n{result.stderr}')
            print('QEMU TCG: same standalone ROM and guest boot probe passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, default=ROOT / 'build/pcemu')
    parser.add_argument('--rom', type=Path, default=ROOT / 'build/firmware/pcemu-vga.bin')
    parser.add_argument('--qemu', action='store_true', help='also validate under qemu-system-i386')
    args = parser.parse_args()
    qemu = shutil.which('qemu-system-i386') if args.qemu else None
    if args.qemu and not qemu:
        parser.error('qemu-system-i386 is required for --qemu')
    run_probe(args.runner.resolve(), args.rom.resolve(), qemu)
