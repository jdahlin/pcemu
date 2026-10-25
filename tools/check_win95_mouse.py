#!/usr/bin/env python3
"""Verify absolute movement and an uncaptured click in a live headless Win95 guest.

Run after check_win95_restart.py --desktop-only. No host GUI input is used.
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('control', type=Path)
    parser.add_argument('frame', type=Path)
    parser.add_argument('state', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    def send(*commands):
        subprocess.run(['python3', str(ROOT/'tools/guest_control.py'), str(args.control),
                        str(args.frame), *commands, '--settle', '0.6'], check=True,
                       stdout=subprocess.DEVNULL)
    def frame():
        header = args.frame.read_bytes().split(b'\n', 3)
        width, height = map(int, header[1].split())
        assert len(header[3]) == width * height * 3
        return width, height, header[3]
    send('key 011b')
    state = json.loads(args.state.read_text())
    assert state['absoluteMouse']['active'], 'guest has not negotiated absolute input'
    width, height, pixels = frame()
    assert width >= 640 and height >= 480
    x, y = width * 2 // 3, height // 3
    nx, ny = x * 65535 // (width - 1), y * 65535 // (height - 1)
    send(f'absolute-mouse {nx} {ny} 0')
    _, _, pixels = frame()
    background = bytes([0, 128, 128])
    points = [(px, py) for py in range(y-3, y+35) for px in range(x-3, x+35)
              if pixels[(py*width+px)*3:(py*width+px)*3+3] != background]
    assert points, 'guest cursor did not reach absolute target'
    bounds = [min(p[0] for p in points), min(p[1] for p in points),
              max(p[0] for p in points), max(p[1] for p in points)]
    assert abs(bounds[0]-x) <= 2 and abs(bounds[1]-y) <= 2, bounds
    (args.output/'absolute-position.ppm').write_bytes(args.frame.read_bytes())
    nx, ny = 15 * 65535 // (width-1), (height-15) * 65535 // (height-1)
    # SDL sends position before the button transition. Exercise the same order.
    send(f'absolute-mouse {nx} {ny} 0')
    send(f'absolute-mouse {nx} {ny} 1')
    send(f'absolute-mouse {nx} {ny} 0')
    _, _, pixels = frame()
    grey = sum(pixels[(py*width+px)*3:(py*width+px)*3+3] == bytes([192]*3)
               for py in range(height-260, height-25) for px in range(3, 164))
    assert grey > 15000, 'absolute left click did not open Start'
    (args.output/'absolute-click.ppm').write_bytes(args.frame.read_bytes())
    state = json.loads(args.state.read_text())
    assert state['absoluteMouse'] == {'active': 1, 'queuedWords': 0}
    send('key 011b')
    nx, ny = x * 65535 // (width-1), y * 65535 // (height-1)
    send(f'absolute-mouse {nx} {ny} 0')
    send(f'absolute-mouse {nx} {ny} 2')
    send(f'absolute-mouse {nx} {ny} 0')
    _, _, pixels = frame()
    grey = sum(pixels[(py*width+px)*3:(py*width+px)*3+3] == bytes([192]*3)
               for py in range(y, min(height,y+160)) for px in range(x, min(width,x+160)))
    assert grey > 5000, 'absolute right click did not open desktop menu'
    (args.output/'absolute-right-click.ppm').write_bytes(args.frame.read_bytes())
    send('key 011b')
    (args.output/'result.json').write_text(json.dumps({
        'mode': [width, height, state['video']['bpp']], 'absoluteActive': True,
        'target': [x,y], 'cursorBounds': bounds, 'absoluteClickOpenedStart': True,
        'absoluteRightClickOpenedMenu': True, 'queueDrained': True, 'hostGuiInput': False}, indent=2)+'\n')
    print('PASS: absolute guest cursor position and Start click verified headless')

if __name__ == '__main__':
    main()
