#!/usr/bin/env python3
"""Send commands to pcemu's append-only guest-test interface and await a capture.
This addresses the emulator's virtual keyboard, not native macOS input.
"""
import argparse
import subprocess
import time
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('control',type=Path)
p.add_argument('frame',type=Path)
p.add_argument('commands',nargs='*',help='quoted commands: key 1c0d, text NAME, scan 38, quit')
p.add_argument('--settle',type=float,default=0.3)
p.add_argument('--png',type=Path)
a=p.parse_args()
with a.control.open('a') as f:
    for command in a.commands:
        f.write(command+'\n')
    f.flush()
if a.commands and a.settle:
    time.sleep(a.settle)
old=a.frame.stat().st_mtime_ns if a.frame.exists() else 0
with a.control.open('a') as f:f.write('capture\n')
end=time.monotonic()+15
while time.monotonic()<end:
    if a.frame.exists() and a.frame.stat().st_mtime_ns>old:
        try:
            data=a.frame.read_bytes();header=data.split(b'\n',3)
            width,height=map(int,header[1].split())
            if header[0]==b'P6' and len(header[3])==width*height*3:
                if a.png:subprocess.run(['sips','-s','format','png',str(a.frame),'--out',str(a.png)],check=True,stdout=subprocess.DEVNULL)
                print(f'Captured {width}x{height} guest framebuffer')
                break
        except (IndexError,ValueError):pass
    time.sleep(0.02)
else:raise SystemExit('No complete new capture within 15 seconds; inspect the existing emulator process before restarting.')
