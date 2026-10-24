#!/usr/bin/env python3
"""Verify a guest-requested Win95 restart on a live headless PCEmu instance.

Reads only the emulator's own framebuffer, RAM metadata, and private FAT disk.
On timeout it leaves the live instance intact for investigation.
"""
import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path
from prepare_media import FAT

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('control', type=Path)
p.add_argument('frame', type=Path)
p.add_argument('memory', type=Path)
p.add_argument('disk', type=Path)
p.add_argument('--pid', type=int, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--timeout', type=float, default=600)
p.add_argument('--desktop-only', action='store_true', help='bootstrap desktop only; no restart or file assertion')
p.add_argument('--restart-only', action='store_true', help='verify restart and desktop without the raw-FAT marker check (supports COW disks)')
a = p.parse_args()
root = Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True, exist_ok=True)
(a.output / 'result.json').unlink(missing_ok=True)

def send(*commands, settle=.25):
    os.kill(a.pid, 0)
    subprocess.run(['python3', str(root / 'tools/guest_control.py'), str(a.control),
                    str(a.frame), *commands, '--settle', str(settle)],
                   check=True, stdout=subprocess.DEVNULL)

def normalize(text):
    return re.sub('[^a-z0-9]', '', text.lower())

def observe():
    send(settle=0)
    state = json.loads(Path(str(a.memory) + '.json').read_text())
    result = subprocess.run(['tesseract', str(a.frame), 'stdout', '--psm', '11'],
                            check=True, text=True, capture_output=True)
    text = normalize(result.stdout)
    header = a.frame.read_bytes().split(b'\n', 3)
    width, height = map(int, header[1].split())
    pixels = header[3]
    # Tesseract's sparse-layout pass rejects text inside some bordered Win95
    # modals. Read their interiors separately in this fixed VGA fixture.
    if (width, height) == (640, 480):
        for index, (x0, y0, x1, y1) in enumerate([
                (180, 228, 505, 275), (40, 265, 402, 415), (180, 170, 480, 270),
                (55, 80, 375, 140), (25, 26, 410, 50)]):
            body = bytearray()
            for y in range(y0, y1):
                row = pixels[(y*width+x0)*3:(y*width+x1)*3]
                doubled = b''.join(row[x:x+3]*2 for x in range(0, len(row), 3))
                body.extend(doubled*2)
            crop = a.output / f'ocr-interior-{index}.ppm'
            crop.write_bytes(f'P6\n{(x1-x0)*2} {(y1-y0)*2}\n255\n'.encode() + body)
            result = subprocess.run(['tesseract', str(crop), 'stdout', '--psm', '6'],
                                    check=True, text=True, capture_output=True)
            text += normalize(result.stdout)
        # Confirm the central desktop is actually unobscured, rather than
        # mistaking visible icons behind an unrecognized modal for readiness.
        samples = [pixels[(y*width+x)*3:(y*width+x)*3+3]
                   for y in range(90, 400, 10) for x in range(130, 600, 10)]
        if sum(pixel == bytes([85, 170, 170]) for pixel in samples) > len(samples)*.95:
            text += 'unobscureddesktop'
    return state, text

def screenshot(name):
    subprocess.run(['sips', '-s', 'format', 'png', str(a.frame), '--out',
                    str(a.output / name)], check=True, stdout=subprocess.DEVNULL)

def desktop(timeout):
    end = time.monotonic() + timeout
    handled = None
    attempts = 0
    while time.monotonic() < end:
        state, text = observe()
        if 'windowsdidnotdetectamouse' in text:
            action = 'mouse warning'
        elif 'networkadapter' in text and 'notworkingproperly' in text:
            action = 'network warning'
        elif 'daylightsaving' in text and ('clock' in text or 'clack' in text):
            action = 'clock warning'
        elif 'datetimeproperties' in text:
            action = 'date/time properties'
        elif 'displayadapter' in text and 'hardwareinstallationwizard' in text:
            action = 'display warning'
        elif 'colorpalette' in text and 'desktoparea' in text:
            action = 'display properties'
        elif 'welcometowindows' in text:
            action = 'welcome'
        elif 'mycomputer' in text and 'ecyclebin' in text and 'unobscureddesktop' in text:
            return state
        else:
            handled = None
            time.sleep(.5)
            continue
        if handled != action:
            print(f'Handling {action}', flush=True)
            handled = action
            attempts = 0
        attempts += 1
        if action == 'clock warning' and attempts == 3:
            send('scan 38', settle=.15)
            send('key 0f09', settle=.2)
            send('scan b8', settle=.2)
        send('key 1c0d' if action in ('mouse warning', 'network warning', 'clock warning')
             else 'key 011b', settle=.8)
    raise RuntimeError('Desktop not verified before timeout; live guest left intact')

first = desktop(a.timeout)
baseline = first['resets']
screenshot('before-restart.png')
print(f'Explorer ready; initial reset count {baseline}', flush=True)
if a.desktop_only:
    (a.output / 'result.json').write_text(json.dumps({
        'scope': 'desktop bootstrap only', 'instructions': first['instructions'],
        'resets': baseline}, indent=2) + '\n')
    print('PASS: unobscured Explorer desktop verified', flush=True)
    raise SystemExit(0)
def request_dialog():
    # Hold the function key as well as Alt; a zero-duration make/break pair
    # is not representative of a physical host shortcut during GUI startup.
    send('scan 38', settle=.2)
    send('scan 3e', settle=.2)
    send('scan be', settle=.15)
    send('scan b8', settle=.2)

request_dialog()
last_shortcut = time.monotonic()
end = time.monotonic() + 30
while time.monotonic() < end:
    _, text = observe()
    if 'restartthecomputer' in text and 'areyousure' in text:
        break
    if 'welcometowindows' in text:
        send('key 011b', settle=.5)
        desktop(a.timeout)
        request_dialog()
        last_shortcut = time.monotonic()
    elif 'unobscureddesktop' in text and time.monotonic() - last_shortcut > 3:
        request_dialog()
        last_shortcut = time.monotonic()
    time.sleep(.3)
else:
    raise RuntimeError('Restart dialog not verified; live guest left intact')
send('key 5000', settle=.3)
send('key 1c0d', settle=.5)
print('Guest Restart requested', flush=True)
end = time.monotonic() + a.timeout
while time.monotonic() < end:
    state, _ = observe()
    if state['resets'] > baseline:
        print(f'Firmware reset observed: {state["resets"]}', flush=True)
        break
    time.sleep(.5)
else:
    raise RuntimeError('Firmware reset not observed; live guest left intact')
last = desktop(a.timeout)
if last['resets'] != baseline + 1:
    raise RuntimeError('Unexpected reset count')
screenshot('after-restart.png')
if a.restart_only:
    (a.output / 'result.json').write_text(json.dumps({
        'scope': 'guest restart and desktop; no file assertion',
        'beforeInstructions': first['instructions'], 'afterInstructions': last['instructions'],
        'beforeResets': baseline, 'afterResets': last['resets']}, indent=2) + '\n')
    print(f'PASS: Explorer returned after guest restart. Evidence: {a.output}', flush=True)
    raise SystemExit(0)
fat = FAT(a.disk)
entry = next(e for e in fat.entries() if e[1] == 'PCEMU95 TXT')
actual = fat.read_file(entry[2], entry[3])
fat.f.close()
expected = b'PCEMU installed Windows 95 passed.'
if actual != expected:
    raise RuntimeError(f'Notepad file did not survive restart: {actual!r}')
proof = {'beforeInstructions': first['instructions'], 'afterInstructions': last['instructions'],
         'beforeResets': baseline, 'afterResets': last['resets'],
         'file': 'C:\\PCEMU95.TXT', 'contents': actual.decode('ascii')}
(a.output / 'result.json').write_text(json.dumps(proof, indent=2) + '\n')
print(f'PASS: Explorer returned after guest restart; exact file survived. Evidence: {a.output}', flush=True)
