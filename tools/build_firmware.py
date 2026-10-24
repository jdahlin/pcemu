#!/usr/bin/env python3
"""Regenerate the checked-in byte arrays for PCEmu's original firmware stubs."""
import subprocess
import tempfile
from pathlib import Path
root=Path(__file__).resolve().parent.parent
for source,header,symbol in [('pci32','pci32','pci32Firmware'),('int15','int15','int15Firmware')]:
    with tempfile.TemporaryDirectory() as folder:
        binary=Path(folder)/'firmware.bin'
        subprocess.run(['nasm','-f','bin',str(root/'firmware'/f'{source}.asm'),'-o',str(binary)],check=True)
        data=binary.read_bytes()
    lines=['// Generated from original firmware/'+source+'.asm with NASM.','#pragma once','#include <cstdint>','namespace pc {','inline constexpr uint8_t '+symbol+'[] = {']
    lines+=['    '+', '.join(f'0x{x:02x}' for x in data[i:i+16])+',' for i in range(0,len(data),16)]
    lines+=['};','} // namespace pc','']
    (root/'src'/f'{header}.hpp').write_text('\n'.join(lines))
