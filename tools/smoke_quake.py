#!/usr/bin/env python3
"""Boot the prepared Quake start map headlessly and verify advancing 3D frames.

Uses the original shareware HUD pixels as a game-specific readiness gate, then
injects forward movement through the emulated keyboard. Keeps failure evidence.
"""
import argparse
import hashlib
import json
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEALTH_SHA256 = 'd94c2571418a13753ac0cc0316b06f3be98f32856bd41dde11b7a81fe8227b75'


def pixels(path):
    parts = path.read_bytes().split(b'\n', 3)
    if parts[:3] != [b'P6', b'320 200', b'255'] or len(parts[3]) != 320 * 200 * 3:
        raise ValueError('Expected a complete 320x200 RGB framebuffer')
    return parts[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, default=ROOT / 'media/quake.qcow2')
    parser.add_argument('--backend', choices=['interpreter', 'jit'], default='jit')
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    subprocess.run(['make', '-s', 'build/pcemu', 'build/disk-tool'], cwd=ROOT, check=True)
    work = Path(tempfile.mkdtemp(prefix='quake-smoke-', dir=ROOT / 'build'))
    disk, control = work / 'guest.qcow2', work / 'control.txt'
    subprocess.run([str(ROOT / 'build/disk-tool'), 'overlay', str(disk), str(args.disk.resolve())], check=True)
    control.write_text('')
    command = [str(ROOT / 'build/pcemu'), '--headless', '--backend', args.backend, '--ram-mb', '32',
               '--disk', str(disk), '--steps', '2000000000', '--capture-every', '25000000',
               '--control-file', str(control), '--frame', str(work / 'live.ppm'),
               '--state-json', str(work / 'live.json')]
    success = False
    captures = []
    previous_instruction = -1
    def send(text):
        with control.open('a') as handle:
            handle.write(text + '\n')
    with (work / 'stdout.txt').open('w') as out, (work / 'stderr.txt').open('w') as err:
        process = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err)
        try:
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('Guest stopped before movement validation')
                try:
                    state = json.loads((work / 'live.json').read_text())
                    instructions = state['instructions']
                    data = pixels(work / 'live.ppm')
                except (FileNotFoundError, ValueError, KeyError):
                    time.sleep(.1)
                    continue
                if instructions <= previous_instruction:
                    time.sleep(.1)
                    continue
                previous_instruction = instructions
                health = b''.join(data[(y * 320 + 136) * 3:(y * 320 + 184) * 3] for y in range(176, 200))
                if hashlib.sha256(health).hexdigest() != HEALTH_SHA256:
                    continue
                if captures and instructions < captures[0]['instructions'] + 100_000_000:
                    continue
                # Freeze before copying the matching state/frame pair. Capture
                # commands are handled on the emulator's main execution thread.
                send('pause\ncapture')
                time.sleep(.2)
                state = json.loads((work / 'live.json').read_text())
                data = pixels(work / 'live.ppm')
                name = f'frame-{len(captures) + 1}.ppm'
                (work / name).write_bytes(b'P6\n320 200\n255\n' + data)
                captures.append({'frame': name, 'instructions': state['instructions'],
                                 'sceneSha256': hashlib.sha256(data[:320 * 152 * 3]).hexdigest(),
                                 'cr0': state['cr0']})
                if len(captures) == 1:
                    send('scan 48\nresume') # Up-arrow make: Quake's default forward key.
                else:
                    send('scan c8\nquit')
                    process.wait(timeout=15)
                    if captures[0]['sceneSha256'] == captures[1]['sceneSha256']:
                        raise RuntimeError('3D scene did not advance after forward input')
                    if process.returncode or not all(c['cr0'] & 1 for c in captures):
                        raise RuntimeError('Guest did not remain in protected-mode gameplay')
                    success = True
                    break
            if not success:
                raise RuntimeError('Timed out waiting for Quake HUD and advancing frames')
        finally:
            if process.poll() is None:
                send('quit')
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            (work / 'report.json').write_text(json.dumps({'success': success, 'command': command,
                'scope': 'original shareware start-map HUD and changing 3D frames after IRQ1 forward input; silent',
                'backend': args.backend,
                'captures': captures}, indent=2) + '\n')
            if success:
                disk.unlink()
                Path(str(disk) + '.idx').unlink(missing_ok=True)
    print(f'PASS: Quake rendered and advanced after keyboard input; evidence: {work}')


if __name__ == '__main__':
    main()
