#!/usr/bin/env python3
"""Boot a private Windows 3.11 copy, save in Notepad, and verify the FAT16 file.
Requires the completed installation in media/win311.raw. No external emulator.
"""
import argparse
import json
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from prepare_media import FAT

root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--disk', type=Path, default=root / 'media/win311.raw')
p.add_argument('--verbose', action='store_true')
a = p.parse_args()
if not shutil.which('tesseract'):
    p.error('This optional integration check requires tesseract (brew install tesseract)')
subprocess.run(['make', '-s'], cwd=root, check=True)
work = Path(tempfile.mkdtemp(prefix='win311-smoke-', dir=root / 'build'))
disk, control, frame = (work / n for n in ['disk.raw', 'control.txt', 'frame.ppm'])
ram = work / 'ram.bin'
subprocess.run(['cp', '-c', str(a.disk.resolve()), str(disk)], check=True)
log = (work / 'emulator.log').open('w')
proc = subprocess.Popen([
    str(root / 'build/pcemu'), '--headless', '--disk', str(disk), '--steps', '0',
    '--type', 'cd windows\nwin /s\n', '--control-file', str(control),
    '--capture-every', '100000000', '--frame', str(frame), '--dump-memory', str(ram),
] + (['--verbose'] if a.verbose else []), cwd=root, stdout=log, stderr=log)

def send(*commands, settle=0.35):
    subprocess.run([
        'python3', str(root / 'tools/guest_control.py'), str(control), str(frame),
        *commands, '--settle', str(settle),
    ], check=True, stdout=subprocess.DEVNULL)

def type_text(text):
    # Real keyboards deliver a stream, rather than flooding the Win16 message
    # queue while a just-created edit control is still receiving focus.
    time.sleep(0.25)
    with control.open('a') as f:
        for char in text:
            f.write('text ' + char + '\n')
            f.flush()
            time.sleep(0.03)
    send(settle=0.3)

def normalize(text):
    return re.sub('[^a-z0-9]', '', text.lower())

def exit_dialog_present():
    # The OCR layout pass omits this bordered modal in the fixed VGA fixture.
    header = frame.read_bytes().split(b'\n', 3)
    width, height = map(int, header[1].split())
    if (width, height) != (640, 480):
        return False
    x0, y0, x1, y1 = 210, 220, 456, 247
    body = b''.join(header[3][(y*width+x0)*3:(y*width+x1)*3] for y in range(y0, y1))
    crop = work / 'exit-dialog-body.ppm'
    crop.write_bytes(f'P6\n{x1-x0} {y1-y0}\n255\n'.encode() + body)
    line = subprocess.run(['tesseract', str(crop), 'stdout', '--psm', '7'],
                          check=True, capture_output=True, text=True).stdout
    return normalize('This will end your Windows session') in normalize(line)

def wait_title(title, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        send(settle=0)
        result = subprocess.run(['tesseract', str(frame), 'stdout', '--psm', '11'],
                                check=True, capture_output=True, text=True).stdout
        if any(normalize(title) in normalize(line) for line in result.splitlines()):
            return
        if title == 'This will end your Windows session' and exit_dialog_present():
            return
        time.sleep(0.1)
    raise RuntimeError(f'Guest did not reach expected title: {title}')

def wait_dos(timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        send(settle=0)
        if frame.read_bytes().split(b'\n', 3)[1] == b'640 400':
            return
        if exit_dialog_present():
            send('key 1c0d', settle=0.2)
        time.sleep(0.1)
    raise RuntimeError('Windows did not exit to text mode')

def alt(scan):
    send('scan 38', f'scan {scan:02x}', f'scan {scan|128:02x}', 'scan b8')

try:
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError('Emulator exited during boot')
        try:
            if json.loads(Path(str(ram) + '.json').read_text())['instructions'] >= 100000000:
                break
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        time.sleep(0.05)
    else:
        raise RuntimeError('No desktop capture within 30 seconds')
    alt(0x21)  # Program Manager: File, Run.
    send('text r')
    wait_title('Run')
    send('text notepad.exe', 'key 1c0d', settle=0.8)
    wait_title('Notepad')
    type_text('PCEMU integration passed.')
    alt(0x21)
    send('text s')
    wait_title('File Name')
    type_text('C:\\PCEMU.TXT')
    send('key 1c0d', settle=0.6)
    wait_title('Notepad PCEMU.TXT')
    alt(0x3e)  # Close Notepad, then Windows, accepting Exit Windows.
    wait_title('Accessories')
    alt(0x3e)
    wait_title('This will end your Windows session')
    send('key 3920', settle=0.8)
    wait_dos()
    send('text smartdrv /c', 'key 1c0d', settle=1)
    with control.open('a') as f:
        f.write('quit\n')
    proc.wait(timeout=10)
    if proc.returncode:
        raise RuntimeError(f'Emulator exited with status {proc.returncode}')
    fat = FAT(disk)
    entry = next(e for e in fat.entries() if e[1] == 'PCEMU   TXT')
    actual = fat.read_file(entry[2], entry[3])
    fat.f.close()
    if actual != b'PCEMU integration passed.':
        raise RuntimeError(f'Guest file mismatch: {actual!r}')
    print(f'PASS: Notepad saved exact contents through Windows and DOS. Evidence: {work}')
finally:
    if proc.poll() is None:
        with control.open('a') as f:
            f.write('quit\n')
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=5)
    log.close()
    print(f'Guest disk and diagnostics retained in {work}')
