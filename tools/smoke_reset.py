#!/usr/bin/env python3
"""Run DOS programs that request reset, then verify a second DOS boot.

Uses private media copies and the emulator's own CPU/devices/VGA ROM path.
"""
import json
import subprocess
import tempfile
from pathlib import Path
from fat_stage import Writer

root = Path(__file__).resolve().parent.parent
subprocess.run(['make', '-s'], cwd=root, check=True)
work = Path(tempfile.mkdtemp(prefix='reset-smoke-', dir=root / 'build'))
programs = {
    'WARM.COM': bytes.fromhex('ea 00 00 ff ff'),
    'KBC.COM': bytes.fromhex('b0 fe e6 64 f4'),
    'FAST.COM': bytes.fromhex('b0 03 e6 92 f4'),
}
for name, code in programs.items():
    disk = work / (name.split('.')[0].lower() + '.raw')
    subprocess.run(['cp', '-c', str(root / 'media/dos.raw'), str(disk)], check=True)
    fat = Writer(disk)
    fat.root_entry(fat.entry(name, 32, fat.allocate(code), len(code)))
    fat.commit()
    fat.f.close()
    ram = disk.with_suffix('.bin')
    command = [str(root / 'build/pcemu'), '--headless', '--execute-vgabios',
               '--disk', str(disk), '--steps', '200000000',
               '--type', name.split('.')[0].lower() + '\n',
               '--frame', str(disk.with_suffix('.ppm')), '--dump-memory', str(ram)]
    with disk.with_suffix('.log').open('w') as log:
        result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=log,
                                text=True, timeout=40, check=True)
    disk.with_suffix('.txt').write_text(result.stdout)
    state = json.loads(Path(str(ram) + '.json').read_text())
    if state['resets'] != 1 or state['cr0'] & 1 or 'PCEMU MS-DOS ready' not in result.stdout:
        raise RuntimeError(f'{name}: second DOS boot not verified; see {work}')
    print(f'PASS: {name} reset and rebooted MS-DOS through VGA ROM', flush=True)
print(f'Evidence retained in {work}')
