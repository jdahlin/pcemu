#!/usr/bin/env python3
"""Build PCEmu's standalone VGA ROM offline with NASM and pinned source fonts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def font_bytes():
    """Map upstream Unicode glyphs into CP437, reversing its LSB-first rows."""
    folder = ROOT / 'firmware/vga/vendor/font8x8'
    manifest = json.loads((folder / 'UPSTREAM.json').read_text())
    glyphs = {}
    for name, digest in manifest['files'].items():
        data = (folder / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'Font source checksum mismatch: {name}')
        for rows, code in re.findall(r'\{([^{}]+)\}[^\n]*// U\+([0-9A-Fa-f]{4})', data.decode()):
            values = re.findall(r'0x[0-9a-fA-F]+', rows)
            if len(values) == 8:
                glyphs[int(code, 16)] = [int(f'{int(v, 16):08b}'[::-1], 2) for v in values]
    # Original supplementary glyphs use VGA's MSB-first row convention. Control
    # code pictures are intentionally deferred; codes 0..31 remain blank.
    extra = {
        236: [0, 0, 0x66, 0x99, 0x99, 0x66, 0, 0],
        239: [0, 0x3c, 0x66, 0x66, 0x66, 0x66, 0, 0],
        240: [0, 0x7e, 0, 0x7e, 0, 0x7e, 0, 0],
        242: [0x60, 0x18, 6, 0x18, 0x60, 0, 0x7e, 0],
        243: [6, 0x18, 0x60, 0x18, 6, 0, 0x7e, 0],
        244: [0x0e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18],
        245: [0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x30, 0xe0],
        247: [0, 0x32, 0x4c, 0, 0x32, 0x4c, 0, 0],
        249: [0, 0, 0, 0x18, 0x18, 0, 0, 0],
        251: [0x0e, 8, 8, 8, 0x68, 0x38, 0x18, 0],
        252: [0x38, 0x24, 0x24, 0x24, 0, 0, 0, 0],
        254: [0, 0, 0x7e, 0x7e, 0x7e, 0x7e, 0, 0],
    }
    result = bytearray()
    for code in range(256):
        result.extend(extra[code] if code in extra else glyphs[ord(bytes([code]).decode('cp437'))])
    return bytes(result)


def build(output):
    """Assemble a 16-KiB option ROM and set its expansion-ROM checksum."""
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    font = font_bytes()
    include = output.parent / 'pcemu-vga-fonts.inc'
    lines = ['; Generated from pinned font8x8 sources; see UPSTREAM.json.']
    for height in (8, 16):
        data = font if height == 8 else bytes(v for v in font for _ in range(2))
        lines.append(f'font_{height}:')
        lines += ['db ' + ','.join(f'0x{v:02x}' for v in data[n:n+16])
                  for n in range(0, len(data), 16)]
    include.write_text('\n'.join(lines) + '\n')
    # NASM 3 warns on absolute 16-bit label relocations even in fixed-origin
    # flat binaries; those relocations are intentional ROM offsets here.
    subprocess.run(['nasm', '-Wall', '-Werror', '-w-reloc-abs-word', '-f', 'bin', '-I', str(output.parent) + '/',
                    str(ROOT / 'firmware/vga/vga.asm'), '-o', str(output)], check=True)
    data = bytearray(output.read_bytes())
    if len(data) != 16384 or data[:3] != bytes([0x55, 0xaa, 32]):
        raise ValueError('Invalid VGA option ROM header/length')
    data[-1] = (-sum(data[:-1])) & 255
    output.write_bytes(data)
    print(f'{output}: {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/firmware/pcemu-vga.bin')
    build(parser.parse_args().output)
