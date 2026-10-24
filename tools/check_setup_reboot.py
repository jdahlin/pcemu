#!/usr/bin/env python3
"""Observe a live Win95 Setup copy phase through its first firmware reboot.

Only acknowledge the explicit Finish/restart screen. Unknown screens and
observation timeouts leave the live guest intact for investigation.
"""
import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('control', type=Path)
p.add_argument('frame', type=Path)
p.add_argument('memory', type=Path)
p.add_argument('--pid', type=int, required=True)
p.add_argument('--before-resets', type=int, default=0)
p.add_argument('--timeout', type=float, default=1800)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
(a.output / 'result.json').unlink(missing_ok=True)
root = Path(__file__).resolve().parent.parent

def send(*commands, settle=.1):
    os.kill(a.pid, 0)
    subprocess.run(['python3', str(root / 'tools/guest_control.py'), str(a.control),
                    str(a.frame), *commands, '--settle', str(settle)],
                   check=True, stdout=subprocess.DEVNULL)

def save(name):
    subprocess.run(['sips', '-s', 'format', 'png', str(a.frame), '--out',
                    str(a.output / name)], check=True, stdout=subprocess.DEVNULL)

end = time.monotonic() + a.timeout
last_progress = None
requested = False
while time.monotonic() < end:
    send()
    state = json.loads(Path(str(a.memory) + '.json').read_text())
    if state['resets'] > a.before_resets:
        if state['resets'] != a.before_resets + 1:
            raise RuntimeError('Unexpected reset count; live guest left intact')
        save('after-setup-reboot.png')
        (a.output / 'result.json').write_text(json.dumps({
            'scope': 'Setup copy phase through first firmware reboot; not installation completion',
            'pid': a.pid, 'beforeResets': a.before_resets, 'afterResets': state['resets'],
            'instructions': state['instructions'], 'finishAcknowledged': requested,
        }, indent=2) + '\n')
        print('PASS: Setup rebooted through firmware in the same process', flush=True)
        break
    result = subprocess.run(['tesseract', str(a.frame), 'stdout', '--psm', '11'],
                            check=True, capture_output=True, text=True)
    text = re.sub('[^a-z0-9]', '', result.stdout.lower())
    progress = re.findall(r'\b(\d{1,3})\s*%', result.stdout)
    if 'copyingfiles' in text and progress and progress[-1] != last_progress:
        last_progress = progress[-1]
        print(f'Setup copying: {last_progress}%', flush=True)
    if not requested and ('restartyourcomputer' in text or 'restartthecomputer' in text) and \
            'finish' in text and ('finishedcopying' in text or 'clickfinish' in text):
        save('before-setup-reboot.png')
        send('scan 1c', settle=.25)
        send('scan 9c', settle=.5)
        requested = True
        print('Acknowledged Setup Finish/restart screen', flush=True)
    time.sleep(2)
else:
    raise RuntimeError('Setup reboot not observed before timeout; live guest left intact')
