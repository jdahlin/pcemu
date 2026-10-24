#!/usr/bin/env python3
"""Inspect linear guest memory in a diagnostic RAM dump and its JSON metadata."""
import argparse
import json
import struct
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('dump', type=Path)
p.add_argument('address', type=lambda x: int(x, 0))
p.add_argument('length', type=lambda x: int(x, 0))
p.add_argument('--out', type=Path, help='write binary instead of a hexadecimal listing')
a = p.parse_args()
b = a.dump.read_bytes()
state = json.loads(Path(str(a.dump) + '.json').read_text())
def word(physical):
    return struct.unpack_from('<I', b, physical)[0]
def physical(linear):
    if not state['cr0'] & 0x80000000:
        return linear
    pde = word((state['cr3'] & ~4095) + (linear >> 22) * 4)
    if not pde & 1:
        raise ValueError(f'Absent PDE for {linear:#x}')
    pte = word((pde & ~4095) + ((linear >> 12) & 1023) * 4)
    if not pte & 1:
        raise ValueError(f'Absent PTE for {linear:#x}')
    return (pte & ~4095) | (linear & 4095)
data = bytes(b[physical(x)] for x in range(a.address, a.address + a.length))
if a.out:
    a.out.write_bytes(data)
else:
    for offset in range(0, len(data), 16):
        line = data[offset:offset+16]
        print(f'{a.address+offset:08x}  {line.hex(" "):<47}  ' +
              ''.join(chr(v) if 32 <= v < 127 else '.' for v in line))
