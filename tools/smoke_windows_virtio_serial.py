#!/usr/bin/env python3
"""Headless signed ARM64 vioserial round trip in disposable Windows PE media.

Uses the existing official boot WIM/ISO and downloaded drivers. Never attaches an
installed system disk. On success removes generated WIM and bootstrap disk files.
"""
import argparse
import hashlib
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
from prepare_win11_boot import prepare
from windows_media_server import make_handler

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=ROOT / 'build/windows11-install/source.iso')
    parser.add_argument('--wim', type=Path, default=ROOT / 'build/windows11-install/boot.wim')
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/pcemu-hvf')
    parser.add_argument('--keep-artifacts', action='store_true')
    args = parser.parse_args()
    iso, source_wim = args.iso.resolve(strict=True), args.wim.resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix='windows-serial-', dir=ROOT / 'build'))
    work.chmod(0o700)
    staging = work / 'staging'
    pcemu = staging / 'PCEMU'
    pcemu.mkdir(parents=True)
    shutil.copytree(ROOT / 'media/netkvm-arm64-0.1.302', pcemu / 'NETKVM')
    shutil.copytree(ROOT / 'media/virtio-arm64-0.1.302/vioserial', pcemu / 'VIOSERIAL')
    for source, name in [('guest-http', 'HTTP'), ('check-serial', 'SERIAL')]:
        shutil.copyfile(ROOT / f'build/windows-guest/{source}.exe', pcemu / f'{name}.EXE')
    (pcemu / 'PROBE.CMD').write_bytes(
        b'@echo off\r\nwpeinit\r\ndrvload X:\\PCEMU\\NETKVM\\netkvm.inf\r\n'
        b'wpeutil InitializeNetwork\r\ndrvload X:\\PCEMU\\VIOSERIAL\\vioser.inf\r\n'
        b'if errorlevel 1 exit /b 1\r\npnputil /enum-devices /connected\r\n'
        b'X:\\PCEMU\\SERIAL.EXE\r\nexit /b %errorlevel%\r\n')
    (pcemu / 'WRAPPER.CMD').write_bytes(
        b'@echo off\r\ncall X:\\PCEMU\\PROBE.CMD > X:\\PCEMU\\PROBE.LOG 2>&1\r\n'
        b'set RC=%errorlevel%\r\necho PCEMU_SERIAL_PROBE_RC=%RC% >> X:\\PCEMU\\PROBE.LOG\r\n'
        b'X:\\PCEMU\\HTTP.EXE post X:\\PCEMU\\PROBE.LOG\r\nwpeutil shutdown\r\n')
    shell = staging / 'winpeshl.ini'
    shell.write_bytes(b'[LaunchApps]\r\n%SYSTEMROOT%\\System32\\cmd.exe, /c X:\\PCEMU\\WRAPPER.CMD\r\n')
    wim, boot = work / 'boot.wim', work / 'boot.raw'
    shutil.copyfile(source_wim, wim)
    with (work / 'staging.log').open('wb') as log:
        commands = f'add "{pcemu}" /PCEMU\nadd "{shell}" /Windows/System32/winpeshl.ini\n'
        subprocess.run(['wimlib-imagex', 'update', str(wim), '2'], input=commands.encode(), stdout=log, stderr=log, check=True)
        subprocess.run(['wimlib-imagex', 'verify', str(wim)], stdout=log, stderr=log, check=True)
    media = prepare(iso, boot, wim)
    (work / 'media.json').write_text(json.dumps(media, indent=2) + '\n')
    reports = work / 'reports'
    socket_path = Path(tempfile.gettempdir()) / f'pcemu-serial-{work.name}.sock'
    command = [str(args.binary), '--machine', 'arm64-uefi', '--cpus', '2', '--ram-mb', '4096',
               '--hyperv', 'vmbus', '--virtio-msix', '--network', 'user', '--disk-controller', 'nvme',
               '--disk', str(boot), '--vars', str(work / 'vars.bin'), '--virtio-serial',
               '--guest-agent-socket', str(socket_path), '--run-forever',
               '--hyperv-stats', str(work / 'devices.json'), '--framebuffer-ppm', str(work / 'console.ppm')]
    server = ThreadingHTTPServer(('127.0.0.1', 8080), make_handler(iso, reports))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    process = None
    try:
        with (work / 'serial.log').open('wb') as out, (work / 'host.log').open('wb') as err:
            process = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err)
            deadline = time.monotonic() + 300
            # A WinPE setup reboot recreates the device and its listening socket.
            # Reconnect until the actual probe announces readiness.
            while True:
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Guest probe did not become ready; inspect host.log and reports')
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(1)
                    try:
                        client.connect(str(socket_path))
                    except (FileNotFoundError, ConnectionRefusedError):
                        time.sleep(0.1)
                        continue
                    data = b''
                    while b'PCEMU_SERIAL_READY\n' not in data and time.monotonic() < deadline:
                        try: chunk = client.recv(4096)
                        except socket.timeout: continue
                        if not chunk: break
                        data += chunk
                        if len(data) > 4096: raise RuntimeError('Unexpected guest serial data')
                    if b'PCEMU_SERIAL_READY\n' not in data:
                        time.sleep(0.1)
                        continue
                    payload = bytes([0, 1, 2, 3, 255, 254, 128, 127, 10, 13, 42, 99, 100, 0, 255, 17])
                    client.sendall(payload[:7]); time.sleep(0.02); client.sendall(payload[7:])
                    while b'PCEMU_SERIAL_ROUNDTRIP_OK\n' not in data:
                        if time.monotonic() > deadline: raise RuntimeError('Serial probe timed out')
                        try: chunk = client.recv(4096)
                        except socket.timeout: continue
                        if not chunk: raise RuntimeError('Serial stream closed before round trip')
                        data += chunk
                        if len(data) > 4096: raise RuntimeError('Unexpected guest serial data')
                    break
            if process.wait(timeout=60) != 0: raise RuntimeError('Windows PE VM failed')
        logs = list(reports.glob('report-*.log'))
        matching = [p for p in logs if b'PCEMU_SERIAL_ROUNDTRIP_OK bytes=16' in p.read_bytes()
                    and b'PCEMU_SERIAL_PROBE_RC=0' in p.read_bytes()]
        if len(matching) != 1: raise RuntimeError('Missing successful Windows serial report')
        stats = json.loads((work / 'devices.json').read_text())['virtioSerial']
        if stats['received'] != 16 or stats['transmitted'] != 45 or not stats['messageInterrupts']:
            raise RuntimeError('Missing serial DMA/MSI-X evidence')
        result = {'verified': True, 'binarySHA256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  'guest': 'Microsoft ARM64 Windows PE 26100.9457', 'bytesToGuest': 16,
                  'guestReport': str(matching[0]), 'serial': stats,
                  'scope': 'Signed vioserial driver and original test process; not a QEMU guest-agent service.'}
        if not args.keep_artifacts:
            for path in (wim, boot, work / 'vars.bin.qcow2', work / 'console.ppm'):
                path.unlink(missing_ok=True)
            shutil.rmtree(staging)
            result['temporaryMediaRemoved'] = True
        (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    finally:
        if process and process.poll() is None:
            process.send_signal(signal.SIGINT)
            try: process.wait(timeout=15)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        server.shutdown(); server.server_close(); thread.join()
        print(f'Artifacts: {work}', flush=True)


if __name__ == '__main__':
    main()
