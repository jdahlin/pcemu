#!/usr/bin/env python3
"""Verify a guest-requested Win95 restart on a live headless PCEmu instance.

Reads only the emulator's own framebuffer, RAM metadata, and private FAT disk.
On timeout it leaves the live instance intact for investigation.
"""
import argparse
import json
import os
import re
import secrets
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
p.add_argument('--applications', action='store_true', help='calculate 2+2, copy result into a fresh Notepad file and restart; caller verifies file after guest exit')
p.add_argument('--application-profile-dir', type=Path, help='diagnostic opcode intervals after Calculator/Notepad have loaded')
p.add_argument('--desktop-only', action='store_true', help='bootstrap desktop only; no restart or file assertion')
p.add_argument('--restart-only', action='store_true', help='verify restart and desktop without the raw-FAT marker check (supports COW disks)')
a = p.parse_args()
if a.application_profile_dir:
    if not a.applications:
        p.error('--application-profile-dir requires --applications')
    a.application_profile_dir = a.application_profile_dir.resolve()
    a.application_profile_dir.mkdir(parents=True, exist_ok=True)
if a.applications:
    if a.desktop_only:
        p.error('--applications requires restart verification')
    a.restart_only = True
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

def read_blue_titles(pixels, width, height):
    # Selected Win95 title text can be omitted/misread by both sparse and
    # whole-window OCR. Find actual blue title bands, then read their bright
    # glyphs against white. This is a second pixel reading, not a fuzzy token.
    bands, current = [], []
    for y in range(height):
        count, left, right = 0, width, 0
        for x in range(width):
            at = (y*width+x)*3
            if pixels[at] < 40 and pixels[at+1] < 40 and pixels[at+2] >= 100:
                count += 1
                left = min(left, x)
                right = max(right, x)
        if count >= 64:
            current.append((y, left, right, count))
        else:
            if 8 <= len(current) <= 24:
                bands.append(current)
            current = []
    if 8 <= len(current) <= 24:
        bands.append(current)
    text = ''
    for index, band in enumerate(sorted(bands, key=lambda b: -sum(r[3] for r in b))[:2]):
        x0, x1 = min(r[1] for r in band), max(r[2] for r in band)+1
        y0, y1 = band[0][0], band[-1][0]+1
        body = bytearray()
        for y in range(y0, y1):
            row = bytearray()
            for x in range(x0, x1):
                pixel = pixels[(y*width+x)*3:(y*width+x)*3+3]
                row.extend((b'\0\0\0' if min(pixel)>230 else b'\xff\xff\xff')*4)
            body.extend(row*4)
        crop = a.output / f'ocr-title-{index}.ppm'
        crop.write_bytes(f'P6\n{(x1-x0)*4} {(y1-y0)*4}\n255\n'.encode()+body)
        result = subprocess.run(['tesseract', str(crop), 'stdout', '--psm', '7'],
                                check=True, text=True, capture_output=True)
        text += normalize(result.stdout)
    return text

def observe(application=False, expected=()):
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
        regions = [(180, 228, 505, 275), (0, 255, 402, 415), (180, 170, 480, 270),
                   (55, 80, 375, 140), (25, 26, 410, 50)]
        if application:
            regions.append((0, 0, width, height))
        for index, (x0, y0, x1, y1) in enumerate(regions):
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
    if application and any(normalize(token) not in text for token in expected):
        text += read_blue_titles(pixels, width, height)
    if width >= 640 and height >= 480:
        # Confirm the central desktop is actually unobscured, rather than
        # mistaking visible icons behind an unrecognized modal for readiness.
        samples = [pixels[(y*width+x)*3:(y*width+x)*3+3]
                   for y in range(90, 400, 10) for x in range(130, 600, 10)]
        if sum(pixel in (bytes([85, 170, 170]), bytes([0, 128, 128])) for pixel in samples) > len(samples)*.95:
            text += 'unobscureddesktop'
            if not application and 'mycomputer' not in text:
                # Fresh Setup leaves My Computer selected. Sparse OCR can
                # discard its white-on-teal label and dotted selection border.
                # Read the actual bright glyphs as black-on-white, retaining
                # both icon-name checks and the independent clear-desktop gate.
                body = bytearray()
                for y in range(35, 55):
                    row = bytearray()
                    for x in range(75):
                        pixel = pixels[(y*width+x)*3:(y*width+x)*3+3]
                        row.extend((b'\0\0\0' if min(pixel) > 230 else b'\xff\xff\xff')*4)
                    body.extend(row*4)
                crop = a.output / 'ocr-selected-icon.ppm'
                crop.write_bytes(b'P6\n300 80\n255\n' + body)
                result = subprocess.run(['tesseract', str(crop), 'stdout', '--psm', '7'],
                                        check=True, text=True, capture_output=True)
                text += normalize(result.stdout)
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
        elif 'dhcp' in text and 'server' in text and ('unable' in text or 'future' in text):
            action = 'DHCP warning'
        elif 'daylightsaving' in text and ('clock' in text or 'clack' in text):
            action = 'clock warning'
        elif 'datetimeproperties' in text:
            action = 'date/time properties'
        elif 'displayadapter' in text and 'hardwareinstallationwizard' in text:
            action = 'display warning'
        elif 'networkpasswordformicrosoft' in text and 'enter' in text:
            # A fresh install with networking can ask for a network login
            # before Explorer. Cancel this known guest-only dialog; validation
            # does not need credentials or a connection to a remote share.
            action = 'network logon'
        elif 'youmustinstallaprinterbeforeyoucanprint' in text:
            action = 'printer setup'
        elif 'configuringyoursystem' in text and 'clickok' in text and 'yourcomputernow' in text:
            # The first-boot configuration explicitly asks to restart before
            # Explorer. Do not acknowledge unrelated or unknown restart screens.
            action = 'setup restart'
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
        if action == 'DHCP warning':
            # No suppresses repeated failure notices on this private test disk.
            send('key 0f09', 'key 1c0d', settle=.8)
            continue
        if action == 'clock warning' and attempts == 3:
            send('scan 38', settle=.15)
            send('key 0f09', settle=.2)
            send('scan b8', settle=.2)
        send('key 1c0d' if action in ('mouse warning', 'network warning', 'clock warning', 'setup restart')
             else 'key 011b', settle=.8)
    raise RuntimeError('Desktop not verified before timeout; live guest left intact')

def dismiss_delayed_welcome():
    # Escape does not reliably close the modeless first-boot window when
    # desktop focus has changed. Close only after identifying its active title.
    for attempt in range(4):
        header = a.frame.read_bytes().split(b'\n', 3)
        width, height = map(int, header[1].split())
        if 'welcome' in read_blue_titles(header[3], width, height):
            shortcut(0x38, 0x3e)  # Alt+F4, with the known Welcome window active.
            return
        shortcut(0x38, 0x0f)  # Alt+Tab; inspect the selected window before closing.
    raise RuntimeError('Could not focus the known delayed Welcome window')

def wait_text(*tokens, absent=(), timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        _, text = observe(application=True, expected=tokens)
        if all(normalize(token) in text for token in tokens) and not any(
                normalize(token) in text for token in absent):
            return
        if 'welcometowindows' in text:
            # First-boot Welcome can appear after Explorer readiness and cover
            # the Run dialog. Dismiss this known popup, then verify the target.
            print('Handling delayed welcome during application check', flush=True)
            dismiss_delayed_welcome()
            if tokens == ('type the name', 'windows will open it'):
                shortcut(0x1d, 0x01)
                send('text r', settle=.3)
            continue
        time.sleep(.2)
    screenshot('application-failure.png')
    raise RuntimeError('Application text not verified: ' + repr(tokens))

def shortcut(modifier, scan):
    send(f'scan {modifier:02x}', settle=.15)
    send(f'scan {scan:02x}', settle=.2)
    send(f'scan {scan | 0x80:02x}', f'scan {modifier | 0x80:02x}', settle=.3)

def launch(program):
    shortcut(0x1d, 0x01)  # Ctrl+Escape opens Start; R invokes Run.
    send('text r', settle=.3)
    wait_text('type the name', 'windows will open it')
    send('text ' + program, 'key 1c0d', settle=.8)

def applications():
    token = secrets.token_hex(4).upper()
    stem = 'P' + token[:7]
    # Classic Calculator's clipboard text reserves a leading sign-space.
    expected = ' 4\r\nPCEMU Win95 applications ' + token + '.'
    print('Checking Calculator and clipboard transfer', flush=True)
    launch('calc.exe')
    wait_text('calculator')
    if a.application_profile_dir:
        send('profile-start', settle=0)
    send('key 011b', 'text 2+2', 'key 1c0d', settle=.6)
    if a.application_profile_dir:
        send('profile-stop ' + str(a.application_profile_dir / 'calculator.json'), settle=0)
    screenshot('calculator.png')
    shortcut(0x1d, 0x2e)  # Ctrl+C: result must come from Calculator.
    shortcut(0x38, 0x3e)
    desktop(a.timeout)
    print('Saving a fresh Notepad file', flush=True)
    launch('notepad.exe')
    wait_text('notepad')
    if a.application_profile_dir:
        send('profile-start', settle=0)
    shortcut(0x1d, 0x2f)  # Ctrl+V: later exact FAT readback verifies arithmetic.
    send('key 1c0d', 'text PCEMU Win95 applications ' + token + '.', settle=.4)
    shortcut(0x38, 0x21)  # File menu; this Win95 Notepad lacks Ctrl+S.
    send('text s', settle=.3)
    wait_text('save', 'name')
    send('text C:\\' + stem + '.TXT', 'key 1c0d', settle=.6)
    wait_text('notepad', absent=('saveas', 'filename'))
    if a.application_profile_dir:
        send('profile-stop ' + str(a.application_profile_dir / 'notepad.json'), settle=0)
    screenshot('notepad-saved.png')
    shortcut(0x38, 0x3e)
    desktop(a.timeout)
    return {'file': 'C:\\' + stem + '.TXT', 'fatName': stem + 'TXT',
            'expectedContents': expected,
            'scope': 'Calculator 2+2 copied through clipboard into a newly saved Notepad file; offline readback pending'}

first = desktop(a.timeout)
baseline = first['resets']
screenshot('before-restart.png')
print(f'Explorer ready; initial reset count {baseline}', flush=True)
apps = applications() if a.applications else None
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
    # Fresh Setup's small MS Sans Serif can render "Are" as "Ale" in OCR.
    if 'restartthecomputer' in text and re.search(r'a[rl]eyousure', text):
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
    result = {'scope': 'guest restart and desktop; no file assertion',
              'beforeInstructions': first['instructions'], 'afterInstructions': last['instructions'],
              'beforeResets': baseline, 'afterResets': last['resets']}
    if apps:
        result['scope'] = 'applications and guest restart; offline file readback pending'
        result['applications'] = apps
    (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
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
