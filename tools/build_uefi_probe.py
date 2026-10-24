#!/usr/bin/env python3
"""Build our tiny PIC UEFI probe without downloading an EFI SDK or linker."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build(output):
    objcopy = shutil.which('llvm-objcopy')
    if not objcopy:
        raise RuntimeError('llvm-objcopy is required')
    output.parent.mkdir(parents=True, exist_ok=True)
    object_path = output.with_suffix('.o')
    code_path = output.with_suffix('.text.bin')
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c',
                    str(ROOT / 'tests/uefi/boot_probe.S'), '-o', str(object_path)], check=True)
    subprocess.run([objcopy, '--dump-section', '.text=' + str(code_path), str(object_path)], check=True)
    code = code_path.read_bytes()
    if len(code) > 512:
        raise RuntimeError('Probe no longer fits its one-section PE layout')
    # PE32+ ARM64, one executable/readable PIC section. No imports, absolute
    # pointers or relocation fixups. UEFI installs protocols and loads this PE.
    header = bytearray(512)
    header[:2] = b'MZ'
    struct.pack_into('<I', header, 60, 0x80)
    header[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', header, 0x84, 0xaa64, 1, 0, 0, 0, 240, 0x22)
    optional = 0x98
    struct.pack_into('<H', header, optional, 0x20b)
    struct.pack_into('<I', header, optional + 4, 512)
    struct.pack_into('<II', header, optional + 16, 0x1000, 0x1000)
    struct.pack_into('<Q', header, optional + 24, 0)
    struct.pack_into('<II', header, optional + 32, 4096, 512)
    struct.pack_into('<II', header, optional + 56, 8192, 512)
    struct.pack_into('<HH', header, optional + 68, 10, 0x100)
    struct.pack_into('<I', header, optional + 108, 16)
    section = optional + 240
    header[section:section + 8] = b'.text\0\0\0'
    struct.pack_into('<IIII', header, section + 8, len(code), 4096, 512, 512)
    struct.pack_into('<I', header, section + 36, 0x60000020)
    output.write_bytes(header + code.ljust(512, b'\0'))
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/uefi-probe/BOOTAA64.EFI')
    args = parser.parse_args()
    print(build(args.output))
