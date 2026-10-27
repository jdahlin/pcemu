#!/usr/bin/env python3
"""Exercise unmodified VGATEST/X-VESA on an existing DOS image; compare QEMU.

Uses small private QCOW2 overlays, no GUI and no new OS installation. Pillow and
Tesseract are validation dependencies. Interactive diagnostics supplement the
independent ROM oracles; this is not a claim of official VGA certification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from PIL import Image, ImageChops, ImageDraw, ImageOps
from cow_file import CowFile
from fat_stage import Writer
from fetch_vga_diagnostics import prepare

ROOT = Path(__file__).resolve().parents[1]


class PcGuest:
    """Own one headless PCEmu process and its capture/control files."""
    def __init__(self, work, disk, rom, runner, no_jit=False):
        self.work, self.control, self.frame = work, work / 'control.txt', work / 'frame.ppm'
        work.mkdir()
        self.log = (work / 'guest.log').open('w')
        self.command = [str(runner), '--headless', '--no-network', '--disk', str(disk),
                        '--vgabios', str(rom), '--execute-vgabios', '--steps', '0',
                        '--control-file', str(self.control), '--frame', str(self.frame),
                        '--state-json', str(work / 'state.json')]
        if no_jit:
            self.command.append('--no-jit')
        self.process = subprocess.Popen(self.command, cwd=ROOT, stdout=self.log,
                                        stderr=subprocess.STDOUT, start_new_session=True)

    def send(self, command):
        """Send virtual input/control, never native host keyboard input."""
        with self.control.open('a') as handle:
            handle.write(command + '\n')

    def text(self, text):
        self.send('resume')
        for index, part in enumerate(text.split('\n')):
            if index:
                self.send('key 1c0d')
            if part:
                self.send('text ' + part)
        time.sleep(.2)

    def space(self):
        self.send('resume')
        self.send('key 3920')
        time.sleep(.3)

    def down(self):
        self.send('resume')
        self.send('key 5000')
        time.sleep(.15)

    def escape(self):
        self.send('resume')
        self.send('key 011b')
        time.sleep(.15)

    def capture(self):
        """Pause before capturing so a diagnostic cannot consume CPU while inspected."""
        old = self.frame.stat().st_mtime_ns if self.frame.exists() else 0
        self.send('pause')
        self.send('capture')
        end = time.monotonic() + 15
        while time.monotonic() < end:
            if self.process.poll() is not None:
                raise RuntimeError(f'PCEmu exited: {(self.work / "guest.log").read_text()[-2000:]}')
            if self.frame.exists() and self.frame.stat().st_mtime_ns > old:
                try:
                    with Image.open(self.frame) as image:
                        return image.convert('RGB').copy()
                except (OSError, ValueError):
                    pass
            time.sleep(.02)
        raise TimeoutError('No PCEmu diagnostic capture')

    def close(self):
        """Shut down the owned guest normally; kill its own group only on timeout."""
        if self.process.poll() is None:
            self.send('quit')
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGTERM)
                self.process.wait(timeout=10)
        self.log.close()


class QemuGuest:
    """Own one independent headless QEMU/SeaBIOS validation guest over QMP."""
    def __init__(self, work, disk, rom):
        work.mkdir()
        self.work, self.frame = work, work / 'frame.ppm'
        self.log = (work / 'guest.log').open('w')
        self.command = ['qemu-system-i386', '-machine', 'pc', '-accel', 'tcg', '-m', '16',
                        '-vga', 'none', '-device', f'VGA,romfile={rom}',
                        '-drive', f'file={disk},format=qcow2,if=ide', '-boot', 'c',
                        '-display', 'none', '-serial', 'none', '-monitor', 'none',
                        '-qmp', f'unix:{work / "qmp.sock"},server=on,wait=off', '-no-reboot']
        self.process = subprocess.Popen(self.command, stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        self.connection = socket.socket(socket.AF_UNIX)
        for _ in range(200):
            try:
                self.connection.connect(str(work / 'qmp.sock'))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if self.process.poll() is not None:
                    raise RuntimeError((work / 'guest.log').read_text())
                time.sleep(.02)
        else:
            raise TimeoutError('QMP did not become available')
        self.connection.settimeout(10)
        self.stream = self.connection.makefile('rwb', buffering=0)
        self.stream.readline()
        self.sequence = 0
        self.command_qmp('qmp_capabilities')

    def command_qmp(self, command, arguments=None):
        """Match replies by ID while ignoring asynchronous QMP state events."""
        self.sequence += 1
        request = {'execute': command, 'id': self.sequence}
        if arguments is not None:
            request['arguments'] = arguments
        self.stream.write(json.dumps(request).encode() + b'\n')
        while True:
            line = self.stream.readline()
            if not line:
                raise RuntimeError('QMP disconnected')
            reply = json.loads(line)
            if reply.get('id') == self.sequence:
                if 'error' in reply:
                    raise RuntimeError(str(reply['error']))
                return reply.get('return')

    def text(self, text):
        self.command_qmp('cont')
        for key in text:
            key = {'\n': 'ret', '.': 'dot', '-': 'minus', '+': 'shift-equal'}.get(key, key)
            self.command_qmp('human-monitor-command', {'command-line': f'sendkey {key}'})
            time.sleep(.08)
        time.sleep(.2)

    def space(self):
        self.command_qmp('cont')
        self.command_qmp('human-monitor-command', {'command-line': 'sendkey spc'})
        time.sleep(.3)

    def down(self):
        self.command_qmp('cont')
        self.command_qmp('human-monitor-command', {'command-line': 'sendkey down'})
        time.sleep(.2)

    def escape(self):
        self.command_qmp('cont')
        self.command_qmp('human-monitor-command', {'command-line': 'sendkey esc'})
        time.sleep(.2)

    def capture(self):
        self.command_qmp('stop')
        self.command_qmp('screendump', {'filename': str(self.frame)})
        with Image.open(self.frame) as image:
            return image.convert('RGB').copy()

    def close(self):
        try:
            if self.process.poll() is None:
                self.command_qmp('quit')
                self.process.wait(timeout=10)
        finally:
            self.stream.close()
            self.connection.close()
            self.log.close()


def ocr(image, path):
    """Use OCR only to recognize menu readiness, never as the pixel oracle."""
    image.save(path)
    return subprocess.run(['tesseract', str(path), 'stdout', '--psm', '6'],
                          capture_output=True, text=True, check=True).stdout.lower()


def pixels(image):
    """Support both current Pillow and versions before get_flattened_data."""
    return image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()


def normalize(image, columns=None, geometry=None, rows=25):
    """Drop VGA's ninth text column; graphics integer scaling preserves pixels."""
    if columns:
        width, height = image.size
        if width == columns * 9:
            output = Image.new('RGB', (columns * 8, height))
            for col in range(columns):
                output.paste(image.crop((col*9, 0, col*9+8, height)), (col*8, 0))
            image = output
        image = image.resize((columns*8, 400), Image.Resampling.NEAREST)
        # VGATEST leaves the cursor at cell 0, with scan lines 14/15 in 25 rows
        # or 6/7 in 50 rows. Exclude only that asynchronous cursor footprint.
        ImageDraw.Draw(image).rectangle((0, (400//rows)-2, 7, (400//rows)-1), fill=(0, 0, 0))
        return image.point(lambda channel: channel >> 2)
    return image.resize(geometry, Image.Resampling.NEAREST).point(lambda channel: channel >> 2)


def run(args):
    """Stage diagnostics once, run static cases, keep evidence and remove owned disks."""
    tools = ROOT / 'build/vga-diagnostics/tools'
    manifest = prepare(tools)
    work = Path(tempfile.mkdtemp(prefix='vga-diagnostics-', dir=ROOT / 'build'))
    rom, runner = work / 'rom.bin', work / 'pcemu-runner'
    shutil.copyfile(args.rom, rom)
    shutil.copyfile(ROOT / 'build/pcemu', runner)
    runner.chmod(0o755)
    seed = work / 'seed.qcow2'
    disk_tool = str(ROOT / 'build/disk-tool')
    subprocess.run([disk_tool, 'overlay', str(seed), str(args.disk.resolve())], check=True)
    with CowFile(seed) as handle:
        fat = Writer(handle)
        fat.replace_existing('CONFIG  SYS', b'DEVICE=C:\\DOS\\HIMEM.SYS /TESTMEM:OFF\r\nFILES=40\r\nBUFFERS=20\r\n')
        fat.replace_existing('AUTOEXECBAT', b'@ECHO OFF\r\nPATH C:\\DOS\r\nPROMPT $P$G\r\nCD \\VGATEST\r\nVGATEST0.EXE\r\nECHO PCEMU-VGATEST-DONE\r\n')
        fat.put_directory('VGATEST', {name: (tools / name).read_bytes()
                                     for name in ('VGATEST0.EXE', 'VGATEST.EXE', 'DOS4GW.EXE', 'X-VESA.COM')})
        handle.flush()
    guests, disks, results = [], [seed], []
    report = {'romSHA256': hashlib.sha256(rom.read_bytes()).hexdigest(),
              'runnerSHA256': hashlib.sha256(runner.read_bytes()).hexdigest(),
              'gitCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'diagnostics': manifest, 'results': results, 'fontMapDifference': 'QEMU clone-style foreground intensity folded to IBM low palette for font-map2 only',
              'comparison': 'Six-bit DAC components; drop ninth text column; exclude text cursor at x0..7, last two glyph scan lines of first row',
              'status': 'running'}
    success = False
    try:
        for kind in (('pcemu', 'qemu') if args.qemu else ('pcemu',)):
            disk = work / f'{kind}.qcow2'
            subprocess.run([disk_tool, 'overlay', str(disk), str(seed)], check=True)
            disks.append(disk)
            guest = PcGuest(work / kind, disk, rom, runner, args.no_jit) if kind == 'pcemu' else QemuGuest(work / kind, disk, rom)
            guests.append(guest)
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                guest.text('')
                time.sleep(.1)
                image = guest.capture()
                if 'which mode' in ocr(image, work / kind / 'ready.png'):
                    break
            else:
                raise TimeoutError(f'{kind}: VGATEST main menu did not appear')
        def keys(text):
            for guest in guests:
                guest.text(text)
        def escape(count=1):
            for _ in range(count):
                for guest in guests:
                    guest.escape()
        def capture(name, columns=None, geometry=None, rows=25, attempt=0):
            images = []
            for guest in guests:
                image = normalize(guest.capture(), columns, geometry, rows)
                if name == 'font-map2' and isinstance(guest, QemuGuest):
                    # IBM 42G2193 reassigns foreground intensity to font selection.
                    # QEMU retains clone-style intensity: fold its bright colors
                    # to the corresponding low palette only for this case.
                    bright = [(21,21,21),(21,21,63),(21,63,21),(21,63,63),
                              (63,21,21),(63,21,63),(63,63,21),(63,63,63)]
                    low = [(0,0,0),(0,0,42),(0,42,0),(0,42,42),
                           (42,0,0),(42,0,42),(42,21,0),(42,42,42)]
                    aliases = dict(zip(bright, low))
                    image.putdata([aliases.get(pixel, pixel) for pixel in pixels(image)])
                image.point(lambda value: (value << 2) | (value >> 4)).save(guest.work / (name + '.png'))
                images.append(image)
            if len(set(pixels(images[0]))) < 2:
                if attempt < 40:
                    for guest in guests:
                        guest.text('')
                    return capture(name, columns, geometry, rows, attempt+1)
                raise RuntimeError(name + ': diagnostic frame is blank')
            record = {'case': name, 'dimensions': images[0].size,
                      'pcemuSHA256': hashlib.sha256(images[0].tobytes()).hexdigest()}
            if len(images) == 2:
                difference = ImageChops.difference(images[0], images[1])
                mismatched = sum(a != b for a, b in zip(pixels(images[0]), pixels(images[1])))
                record['differentPixels'] = mismatched
                record['qemuSHA256'] = hashlib.sha256(images[1].tobytes()).hexdigest()
                if mismatched and attempt < 40:
                    # PCEmu's saved frames use a visible blink phase; QEMU's
                    # live capture may land in either phase. Retry for one
                    # bounded drawing/blink window, with every pixel still checked.
                    for guest in guests:
                        guest.text('')
                    return capture(name, columns, geometry, rows, attempt+1)
                if mismatched:
                    difference.save(work / (name + '-difference.png'))
                    results.append(record)
                    raise RuntimeError(f'{name}: {mismatched} pixels differ from independent QEMU scanout')
            results.append(record)
            print(f'{name}: passed' + (' exact QEMU pixel comparison' if args.qemu else ' diagnostic exercise'), flush=True)
        keys('t'); keys('f'); keys('c3'); capture('font-map0', columns=80)
        keys('2'); capture('font-map2', columns=80)
        escape(); keys('1b'); capture('font-80x50', columns=80, rows=50)
        escape(3); keys('g'); keys('t')
        for mode, geometry in [('04',(320,200)),('06',(640,200)),('0d',(320,200)),
                               ('12',(640,480)),('13',(320,200)),('18',(320,240))]:
            keys(mode); capture('test-card-'+mode, geometry=geometry); escape()
        escape(2); keys('q'); keys('x-vesa.com\n')
        time.sleep(2)
        capture('xvesa-enumeration', columns=80, rows=50)
        keys('\n'); keys('\n')
        time.sleep(.5)
        for guest in guests:
            image = guest.capture()
            content = ocr(image.resize((1280,800)), guest.work / 'xvesa-mode-info.png')
            (guest.work / 'xvesa-mode-info.txt').write_text(content)
            required = ('video mode information', 'granularity', 'horizontal resolution', 'linear physical address')
            if not all(term in content for term in required):
                raise RuntimeError(type(guest).__name__ + ': X-VESA mode information missing')
        results.append({'case': 'xvesa-mode-information', 'status': 'passed',
                        'note': 'Decoded fields checked by OCR; platform PCI BARs legitimately differ'})
        escape()
        for _ in range(7):
            for guest in guests:
                guest.down()
        keys('\n')
        time.sleep(.5)
        for guest in guests:
            image = guest.capture()
            image.save(guest.work / 'xvesa-dac-entry.png')
            content = ocr(ImageOps.grayscale(image.crop((32,160,610,225))).point(lambda value: 0 if value > 128 else 255).resize((1156,130)), guest.work / 'xvesa-dac-prompt.png')
            if 'unavailable' not in content:
                raise RuntimeError('Expected X-VESA unchanged-legacy-register heuristic prompt')
        # DISPI mode selection leaves the legacy VGA registers intact on both
        # boards. X-VESA's heuristic asks to continue; this is recorded explicitly.
        keys('y')
        capture('xvesa-dac-initial', geometry=(640,400))
        for guest in guests:
            guest.space()
        capture('xvesa-dac-toggled', geometry=(640,400))
        if results[-1]['pcemuSHA256'] == results[-2]['pcemuSHA256']:
            raise RuntimeError('X-VESA DAC toggle did not change the gradient')
        state = json.loads((guests[0].work / 'state.json').read_text())
        if not (state['video']['dispiEnabled'] & 0x20):
            raise RuntimeError('X-VESA did not enable native 8-bit DAC precision')
        results.append({'case':'xvesa-dac-entry', 'note':'Accepted identical unchanged-legacy-register heuristic on both DISPI boards'})
        success = True
        report['status'] = 'passed'
    except BaseException as error:
        report['status'], report['error'] = 'failed', str(error)
        raise
    finally:
        for guest in reversed(guests):
            guest.close()
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        if success:
            for disk in reversed(disks):
                disk.unlink(missing_ok=True)
            for guest in guests:
                (guest.work / 'ram.bin').unlink(missing_ok=True)
            rom.unlink(missing_ok=True)
            runner.unlink(missing_ok=True)
        print('Evidence:', work, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, required=True, help='existing local QCOW2 DOS base (read-only parent)')
    parser.add_argument('--rom', type=Path, default=ROOT / 'build/firmware/pcemu-vga.bin')
    parser.add_argument('--no-jit', action='store_true')
    parser.add_argument('--qemu', action='store_true', help='require identical normalized QEMU scanout')
    args = parser.parse_args()
    if not args.disk.is_file():
        parser.error('existing DOS image required')
    run(args)
