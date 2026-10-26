#!/usr/bin/env python3
"""Verify VirtIO guest-tool devices on a private installed Windows ARM64 overlay.

A non-formatting WinPE pass adds the signed GPU package and one-shot probes.
The installed guest then verifies serial I/O, balloon inflation and real GPU
pixels, removes its temporary autologon password, posts reports and shuts down.
The parent is never opened writable. Successful test disks remain for review.
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


def cmdfile(path, text):
    path.write_bytes(text.replace('\n', '\r\n').encode('ascii'))


def private(path, data):
    with path.open('xb') as out:
        path.chmod(0o600)
        out.write(data)


def wait_vm(process, timeout):
    try:
        if process.wait(timeout=timeout): raise RuntimeError('VM failed; inspect host log')
    except subprocess.TimeoutExpired:
        raise RuntimeError('VM did not power off before validation timeout')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parent', type=Path, default=ROOT / 'media/windows11-arm64/system-live.qcow2')
    parser.add_argument('--iso', type=Path, default=ROOT / 'build/windows11-install/source.iso')
    parser.add_argument('--wim', type=Path, default=ROOT / 'build/windows11-install/boot.wim')
    parser.add_argument('--credentials', type=Path, default=ROOT / 'media/windows11-arm64/local-account.json')
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/pcemu-hvf')
    args = parser.parse_args()
    parent, iso, source_wim, binary = [p.resolve(strict=True) for p in (args.parent, args.iso, args.wim, args.binary)]
    account = json.loads(args.credentials.read_text())
    if account['username'] != 'PCEmu' or any(c in account['password'] for c in '\r\n\0'):
        parser.error('Expected the generated local PCEmu validation account')
    work = Path(tempfile.mkdtemp(prefix='windows-tools-', dir=ROOT / 'build'))
    work.chmod(0o700)
    disk = work / 'system.qcow2'
    subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', 'qcow2', '-b', str(parent), str(disk)], check=True)
    disk.chmod(0o600)
    staging = work / 'staging'
    pcemu = staging / 'PCEMU'
    pcemu.mkdir(parents=True)
    shutil.copytree(ROOT / 'media/netkvm-arm64-0.1.302', pcemu / 'NETKVM')
    shutil.copytree(ROOT / 'media/viostor-arm64-intx-0.1.285/viostor', pcemu / 'VIOSTOR')
    shutil.copytree(ROOT / 'media/virtio-arm64-complete-0.1.302/viogpudo', pcemu / 'GPU')
    for source, name in [('guest-http', 'HTTP'), ('check-serial', 'SERIAL'), ('check-display', 'DISPLAY')]:
        shutil.copyfile(ROOT / f'build/windows-guest/{source}.exe', pcemu / f'{name}.EXE')
    shutil.copyfile(ROOT / 'tools/windows/finalize_mount.txt', pcemu / 'MOUNT.TXT')
    shutil.copyfile(ROOT / 'tools/windows/prepare_probe_logon.cmd', pcemu / 'PREPARE-LOGON.CMD')
    password = account['password'].replace('\\', '\\\\').replace('"', '\\"')
    credentials = ('Windows Registry Editor Version 5.00\n\n'
                   '[HKEY_LOCAL_MACHINE\\PCEMU_SOFT\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon]\n'
                   '"DefaultUserName"="PCEmu"\n'
                   f'"DefaultPassword"="{password}"\n'
                   '"AutoAdminLogon"="1"\n')
    private(pcemu / 'ACCOUNT.REG', credentials.replace('\n', '\r\n').encode('utf-16'))
    del password, credentials, account
    cmdfile(pcemu / 'STAGE.CMD', r'''@echo off
wpeinit
drvload X:\PCEMU\NETKVM\netkvm.inf
if errorlevel 1 exit /b 1
drvload X:\PCEMU\VIOSTOR\viostor.inf
if errorlevel 1 exit /b 1
wpeutil InitializeNetwork
diskpart /s X:\PCEMU\MOUNT.TXT
if errorlevel 1 exit /b 1
if not exist W:\Windows\System32\ntoskrnl.exe exit /b 1
if not exist W:\PCEMU\FIRSTBOOT.CMD exit /b 1
dism /Image:W:\ /Add-Driver /Driver:X:\PCEMU\GPU\viogpudo.inf
if errorlevel 1 exit /b 1
copy /y X:\PCEMU\HTTP.EXE W:\PCEMU\TOOLS-HTTP.EXE
if errorlevel 1 exit /b 1
copy /y X:\PCEMU\SERIAL.EXE W:\PCEMU\TOOLS-SERIAL.EXE
if errorlevel 1 exit /b 1
copy /y X:\PCEMU\DISPLAY.EXE W:\PCEMU\TOOLS-DISPLAY.EXE
if errorlevel 1 exit /b 1
copy /y X:\PCEMU\PROBE.CMD W:\PCEMU\TOOLS-PROBE.CMD
if errorlevel 1 exit /b 1
copy /y X:\PCEMU\PROBE-WRAPPER.CMD W:\PCEMU\TOOLS-WRAPPER.CMD
if errorlevel 1 exit /b 1
reg load HKLM\PCEMU_SOFT W:\Windows\System32\config\SOFTWARE
if errorlevel 1 exit /b 1
reg import X:\PCEMU\ACCOUNT.REG >nul 2>&1
if errorlevel 1 goto unloadfail
call X:\PCEMU\PREPARE-LOGON.CMD
if errorlevel 1 goto unloadfail
reg add HKLM\PCEMU_SOFT\Microsoft\Windows\CurrentVersion\RunOnce /v PCEMU_TOOLS /t REG_SZ /d "cmd.exe /c C:\PCEMU\TOOLS-WRAPPER.CMD > C:\PCEMU\TOOLS.LOG 2>&1" /f
if errorlevel 1 goto unloadfail
reg unload HKLM\PCEMU_SOFT
if errorlevel 1 exit /b 1
echo PCEMU_TOOLS_STAGED_OK
exit /b 0
:unloadfail
reg unload HKLM\PCEMU_SOFT
exit /b 1
''')
    cmdfile(pcemu / 'STAGE-WRAPPER.CMD', r'''@echo off
call X:\PCEMU\STAGE.CMD > X:\PCEMU\STAGE.LOG 2>&1
set RC=%errorlevel%
echo PCEMU_TOOLS_STAGE_RC=%RC% >> X:\PCEMU\STAGE.LOG
X:\PCEMU\HTTP.EXE post X:\PCEMU\STAGE.LOG
wpeutil shutdown
''')
    cmdfile(pcemu / 'PROBE.CMD', r'''@echo off
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 0 /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultPassword /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoLogonCount /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v ForceAutoLogon /f >nul 2>&1
echo PCEMU_TOOLS_INSTALLED_WINDOWS_BEGIN
ver
whoami
start "" /b C:\PCEMU\TOOLS-DISPLAY.EXE > C:\PCEMU\TOOLS-DISPLAY.LOG 2>&1
pnputil /enum-devices /connected
compact.exe /CompactOS:query
C:\PCEMU\TOOLS-SERIAL.EXE
exit /b %errorlevel%
''')
    cmdfile(pcemu / 'PROBE-WRAPPER.CMD', r'''@echo off
call C:\PCEMU\TOOLS-PROBE.CMD
set RC=%errorlevel%
echo PCEMU_TOOLS_INSTALLED_RC=%RC%
C:\PCEMU\TOOLS-HTTP.EXE post C:\PCEMU\TOOLS.LOG
C:\PCEMU\TOOLS-HTTP.EXE post C:\PCEMU\TOOLS-DISPLAY.LOG
shutdown.exe /s /t 0
''')
    shell = staging / 'winpeshl.ini'
    cmdfile(shell, '[LaunchApps]\n%SYSTEMROOT%\\System32\\cmd.exe, /c X:\\PCEMU\\STAGE-WRAPPER.CMD\n')
    wim, boot = work / 'boot.wim', work / 'boot.raw'
    shutil.copyfile(source_wim, wim)
    wim.chmod(0o600)
    with (work / 'staging.log').open('wb') as log:
        commands = f'add "{pcemu}" /PCEMU\nadd "{shell}" /Windows/System32/winpeshl.ini\n'
        subprocess.run(['wimlib-imagex', 'update', str(wim), '2'], input=commands.encode(), stdout=log, stderr=log, check=True)
        subprocess.run(['wimlib-imagex', 'verify', str(wim)], stdout=log, stderr=log, check=True)
    prepare(iso, boot, wim)
    boot.chmod(0o600)
    reports = work / 'reports'
    server = ThreadingHTTPServer(('127.0.0.1', 8080), make_handler(iso, reports))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    common = [str(binary), '--machine', 'arm64-uefi', '--cpus', '2', '--ram-mb', '4096',
              '--hyperv', 'vmbus', '--virtio-msix', '--network', 'user', '--run-forever']
    process = None
    try:
        command = common + ['--disk-controller', 'nvme', '--disk', str(boot), '--vars', str(work / 'stage-vars.bin'),
                            '--extra-disk', str(parent), '--extra-disk-overlay', str(disk)]
        with (work / 'stage-serial.log').open('wb') as out, (work / 'stage-host.log').open('wb') as err:
            process = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err)
            wait_vm(process, 180)
        stage_reports = [p for p in reports.glob('report-*') if b'PCEMU_TOOLS_STAGED_OK' in p.read_bytes()
                         and b'PCEMU_TOOLS_STAGE_RC=0' in p.read_bytes()]
        if len(stage_reports) != 1: raise RuntimeError('Offline guest-tools staging failed; inspect reports')
        # The emulator requires an overlay's immediate parent to match --disk.
        socket_path = Path(tempfile.gettempdir()) / f'pcemu-tools-{work.name}.sock'
        shutil.copyfile(ROOT / 'build/windows11-qcow2/installed-vars.bin', work / 'vars.bin')
        command = common + ['--disk-controller', 'virtio', '--disk', str(parent), '--disk-overlay', str(disk),
                            '--vars', str(work / 'vars.bin'), '--virtio-serial', '--guest-agent-socket', str(socket_path),
                            '--virtio-balloon', '--balloon-pfn64', '--balloon-target-mb', '64', '--virtio-gpu',
                            '--hyperv-stats', str(work / 'devices.json'), '--framebuffer-ppm', str(work / 'console.ppm')]
        gpu_observed = balloon_observed = None
        from PIL import Image
        with (work / 'serial.log').open('wb') as out, (work / 'host.log').open('wb') as err:
            process = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err)
            deadline = time.monotonic() + 300
            while not socket_path.exists():
                if process.poll() is not None or time.monotonic() > deadline: raise RuntimeError('Installed VM did not initialize')
                time.sleep(0.1)
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(1); client.connect(str(socket_path)); data = b''
                while b'PCEMU_SERIAL_READY\n' not in data:
                    if process.poll() is not None or time.monotonic() > deadline: raise RuntimeError('Installed serial probe did not open')
                    try: chunk = client.recv(4096)
                    except socket.timeout: continue
                    if not chunk: raise RuntimeError('Installed serial port closed before probe')
                    data += chunk
                    if len(data) > 4096: raise RuntimeError('Unexpected serial data')
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    process.send_signal(signal.SIGUSR1); time.sleep(1)
                    try: stats = json.loads((work / 'devices.json').read_text())
                    except (OSError, json.JSONDecodeError): continue
                    (work / 'live-devices.json').write_text(json.dumps(stats, indent=2) + '\n')
                    balloon, gpu = stats.get('virtioBalloon', {}), stats.get('virtioGpu', {})
                    if balloon.get('actualPages') == 16384 and balloon.get('inflatedPages') == 16384: balloon_observed = balloon
                    if gpu.get('active') and gpu.get('transfers') and gpu.get('flushes'):
                        try:
                            picture = Image.open(work / 'console.ppm').convert('RGB')
                            width, height = picture.size
                            colors = [picture.getpixel((width * x // 6, height // 2)) for x in (1, 3, 5)]
                            if colors == [(255, 0, 0), (0, 255, 0), (0, 0, 255)]:
                                gpu_observed = {**gpu, 'width': width, 'height': height, 'sampleRGB': colors}
                                picture.save(work / 'gpu.png')
                        except (OSError, ValueError): pass
                    if gpu_observed and balloon_observed: break
                payload = bytes([0, 1, 2, 3, 255, 254, 128, 127, 10, 13, 42, 99, 100, 0, 255, 17])
                client.sendall(payload[:7]); time.sleep(0.02); client.sendall(payload[7:])
                deadline = time.monotonic() + 30
                while b'PCEMU_SERIAL_ROUNDTRIP_OK\n' not in data:
                    if time.monotonic() > deadline: raise RuntimeError('Installed serial round trip timed out')
                    try: chunk = client.recv(4096)
                    except socket.timeout: continue
                    if not chunk: raise RuntimeError('Installed serial probe closed early')
                    data += chunk
            wait_vm(process, 90)
        logs = list(reports.glob('report-*'))
        accepted = [p for p in logs if b'PCEMU_TOOLS_INSTALLED_RC=0' in p.read_bytes()
                    and b'PCEMU_SERIAL_ROUNDTRIP_OK bytes=16' in p.read_bytes()]
        if len(accepted) != 1: raise RuntimeError('Missing successful installed Windows report')
        if not gpu_observed: raise RuntimeError('No confirmed VirtIO GPU RGB pattern; inspect live-devices and display log')
        if not balloon_observed: raise RuntimeError('Installed balloon did not reach 64 MiB')
        subprocess.run(['qemu-img', 'check', str(disk)], check=True)
        final = json.loads((work / 'devices.json').read_text())
        result = {'verified': True, 'parent': str(parent), 'disk': str(disk),
                  'binarySHA256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                  'guestReport': str(accepted[0]), 'gpu': gpu_observed, 'balloon': balloon_observed,
                  'finalSerial': final['virtioSerial'], 'finalBalloon': final['virtioBalloon'],
                  'scope': 'Signed ARM64 device drivers on installed Windows; original test programs, not QEMU guest-agent or SPICE services.'}
        # Remove generated boot media and credential-bearing staging after success.
        for path in (wim, boot, work / 'stage-vars.bin.qcow2', work / 'console.ppm'):
            path.unlink(missing_ok=True)
        shutil.rmtree(staging)
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
