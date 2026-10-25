#!/usr/bin/env python3
"""Time the unchanged PCEMU CoreMark COM under headless QEMU TCG.

QEMU lacks PCEMU's host-clock port 0xe8. Fixed iterations and canonical CRCs
are checked, but upstream duration validation is unavailable: no official score.
Socket receipt timestamps bracket guest port 0xe7 markers, excluding BIOS/load.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def validate(text, iterations):
    for field, value in [('seedcrc', 'e9f5'), ('crclist', 'e714'), ('crcmatrix', '1fd7'), ('crcstate', '8e3a')]:
        if not re.search(rf'{field}\s+: 0x{value}\b', text):
            raise ValueError('Missing canonical ' + field)
    checked = text.replace('ERROR! Must execute for at least 10 secs for a valid result!', '')
    if 'ERROR' in checked or 'Cannot validate' in text or not re.search(rf'Iterations\s+: {iterations}\b', text):
        raise ValueError('Guest CRC/iteration validation failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--iterations', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    if args.iterations <= 0 or args.timeout <= 0:
        parser.error('Positive iterations and timeout required')
    qemu = shutil.which('qemu-system-i386')
    if not qemu:
        parser.error('qemu-system-i386 required')
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    program = args.program.read_bytes()
    if not 0 < len(program) <= 65000:
        parser.error('Expected a COM fitting one segment')
    sectors = (len(program) + 511) // 512
    subprocess.run(['nasm', '-f', 'bin', f'-DSECTORS={sectors}',
                    str(ROOT / 'tests/guest_perf/qemu_boot.asm'),
                    '-o', str(work / 'boot.bin')], check=True)
    disk = work / 'boot.raw'
    disk.write_bytes((work / 'boot.bin').read_bytes() + program +
                     bytes(sectors * 512 - len(program)))
    with disk.open('r+b') as handle:
        handle.truncate(1024 * 1024)  # Sparse tail; no VM installation disk.
    before_load = os.getloadavg()
    with tempfile.TemporaryDirectory(prefix='pcemu-qemu-') as temporary:
        with socket.socket(socket.AF_UNIX) as listener:
            endpoint = str(Path(temporary) / 'markers')
            listener.bind(endpoint)
            listener.listen(1)
            listener.settimeout(args.timeout)
            command = [qemu, '-machine', 'pc', '-accel', 'tcg,thread=single',
                       '-cpu', '486', '-smp', '1', '-m', '16', '-display', 'none',
                       '-monitor', 'none', '-serial', 'none', '-nic', 'none',
                       '-no-reboot', '-drive', f'file={disk},format=raw',
                       '-chardev', f'socket,id=markers,path={endpoint}',
                       '-device', 'isa-debugcon,iobase=0xe7,chardev=markers',
                       '-chardev', f'file,id=text,path={work / "guest.txt"}',
                       '-device', 'isa-debugcon,iobase=0xe9,chardev=text']
            with (work / 'qemu.log').open('w') as log:
                guest = subprocess.Popen(command, stdout=log, stderr=log)
                markers = []
                deadline = time.monotonic() + args.timeout
                try:
                    connection, _ = listener.accept()
                    with connection:
                        for expected in (1, 2, 3):
                            connection.settimeout(max(.001, deadline - time.monotonic()))
                            value = connection.recv(1)
                            stamp = time.monotonic_ns()
                            if value != bytes([expected]):
                                raise RuntimeError(f'Expected marker {expected}, received {value!r}')
                            markers.append(stamp)
                finally:
                    if guest.poll() is None:
                        guest.terminate()
                    try:
                        guest.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        guest.kill()
                        guest.wait()
    transcript = (work / 'guest.txt').read_text()
    validate(transcript, args.iterations)
    seconds = (markers[1] - markers[0]) / 1e9
    result = {'scope': __doc__, 'command': command,
              'qemuVersion': subprocess.check_output([qemu, '--version'], text=True),
              'qemuSha256': hashlib.sha256(Path(qemu).read_bytes()).hexdigest(),
              'programSha256': hashlib.sha256(program).hexdigest(),
              'host': {'machine': platform.machine(), 'release': platform.release(),
                       'loadBefore': before_load, 'loadAfter': os.getloadavg()},
              'iterations': args.iterations, 'hostSeconds': seconds,
              'iterationsPerHostSecond': args.iterations / seconds,
              'markerReceiptNanoseconds': markers, 'crcValidated': True,
              'publishedScoreEligible': False,
              'upstreamDurationClockAvailable': False,
              'guestSha256': hashlib.sha256(transcript.encode()).hexdigest()}
    disk.unlink()  # Remove only the successfully validated tiny private disk.
    result['privateDiskRemoved'] = True
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS: {seconds:.6f}s, canonical CRCs; {work}', flush=True)


if __name__ == '__main__':
    main()
