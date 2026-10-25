#!/usr/bin/env python3
"""Original dependent-load sweep, not an lmbench port or official lmbench score.

Same guest COM for a CPU/JIT-only PCEMU harness and QEMU's full PC TCG.
Supervisor flat/identity-paged 4KiB RAM, no measured disk activity. QEMU uses
host socket-receipt timing; PCEMU timestamps in its port handler. Reported
ns/load includes guest execution, guards and dispatch, not raw host DRAM latency.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SIZES = (16, 256, 1024, 2048, 4096)
CASES = [(paged, stride, nodes) for paged in (0, 1)
         for stride in (4, 4096) for nodes in SIZES]


def permutation(nodes):
    order = list(range(nodes))
    state = 0x12345678
    for i in range(nodes - 1, 0, -1):
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        j = state % (i + 1)
        order[i], order[j] = order[j], order[i]
    return order


def validate(text, loops):
    lines = text.splitlines()
    if len(lines) != len(CASES):
        raise ValueError('Expected exactly 20 completed cases')
    records = []
    for line, case in zip(lines, CASES):
        fields = line.split()
        if len(fields) != 4:
            raise ValueError('Expected four result fields')
        values = [int(field, 16) for field in fields]
        if tuple(values[:3]) != case:
            raise ValueError('Case order/metadata mismatch')
        paged, stride, nodes = case
        loads = loops * 32 + 7
        order = permutation(nodes)
        expected = 0x400000 + order[loads % nodes] * stride
        if values[3] != expected:
            raise ValueError(f'Pointer result {values[3]:x}, expected {expected:x}')
        records.append({'paged': bool(paged), 'strideBytes': stride, 'nodes': nodes,
                        'addressSpanBytes': (nodes - 1) * stride + 4,
                        'guestPagesTouched': ((nodes - 1) * stride + 4 + 4095) // 4096,
                        'dependentLoads': loads, 'finalPointer': values[3]})
    return records


def build(work, loops):
    payload = work / 'memory.bin'
    program = work / 'MEMORY.COM'
    subprocess.run(['nasm', '-f', 'bin', f'-DLOOPS={loops}',
                    str(ROOT / 'tests/guest_perf/memory32.asm'), '-o', str(payload)], check=True)
    subprocess.run(['nasm', '-f', 'bin', str(ROOT / 'tests/guest_perf/dos_loader.asm'),
                    f'-DPAYLOAD="{payload}"', '-o', str(program)], check=True)
    if program.stat().st_size > 65000:
        raise ValueError('COM exceeds a segment')
    return program


def run_qemu(work, program, timeout):
    qemu = shutil.which('qemu-system-i386')
    if not qemu:
        raise ValueError('qemu-system-i386 required')
    blob = program.read_bytes()
    sectors = (len(blob) + 511) // 512
    boot = work / 'boot.bin'
    subprocess.run(['nasm', '-f', 'bin', f'-DSECTORS={sectors}',
                    str(ROOT / 'tests/guest_perf/qemu_boot.asm'), '-o', str(boot)], check=True)
    disk = work / 'boot.raw'
    disk.write_bytes(boot.read_bytes() + blob + bytes(sectors * 512 - len(blob)))
    with disk.open('r+b') as handle:
        handle.truncate(1024 * 1024)
    samples = []
    with tempfile.TemporaryDirectory(prefix='pcemu-mem-') as temporary:
        with socket.socket(socket.AF_UNIX) as listener:
            path = str(Path(temporary) / 'markers')
            listener.bind(path); listener.listen(1); listener.settimeout(timeout)
            command = [qemu, '-machine', 'pc', '-accel', 'tcg,thread=single',
                       '-cpu', '486', '-smp', '1', '-m', '64', '-display', 'none',
                       '-monitor', 'none', '-serial', 'none', '-nic', 'none', '-no-reboot',
                       '-drive', f'file={disk},format=raw',
                       '-chardev', f'socket,id=markers,path={path}',
                       '-device', 'isa-debugcon,iobase=0xe7,chardev=markers',
                       '-chardev', f'file,id=text,path={work / "guest.txt"}',
                       '-device', 'isa-debugcon,iobase=0xe9,chardev=text']
            with (work / 'qemu.log').open('w') as log:
                guest = subprocess.Popen(command, stdout=log, stderr=log)
                deadline = time.monotonic() + timeout
                try:
                    listener.settimeout(.2)
                    while True:
                        try:
                            connection, _ = listener.accept()
                            break
                        except socket.timeout:
                            if guest.poll() is not None:
                                raise RuntimeError('QEMU exited before connecting; inspect qemu.log')
                            if time.monotonic() >= deadline:
                                raise TimeoutError('QEMU connection timeout')
                    with connection:
                        def marker(expected):
                            connection.settimeout(max(.001, deadline - time.monotonic()))
                            value = connection.recv(1)
                            stamp = time.monotonic_ns()
                            if value != bytes([expected]):
                                raise RuntimeError(f'Expected marker {expected}, got {value!r}')
                            return stamp
                        for _ in CASES:
                            start = marker(1); stop = marker(2)
                            samples.append({'hostSeconds': (stop - start) / 1e9,
                                            'markerReceiptNanoseconds': [start, stop]})
                        marker(3)
                finally:
                    if guest.poll() is None:
                        guest.terminate()
                    try:
                        guest.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        guest.kill(); guest.wait()
    return {'samples': samples, 'command': command,
            'qemuVersion': subprocess.check_output([qemu, '--version'], text=True),
            'runnerSha256': hashlib.sha256(Path(qemu).read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=['jit', 'interpreter', 'qemu'], default='jit')
    parser.add_argument('--loops', type=int, default=4000000)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--runner', type=Path, help='Existing PCEMU memory-bench executable; skip rebuild')
    args = parser.parse_args()
    if not 1 <= args.loops <= 10000000 or args.timeout <= 0:
        parser.error('loops must be 1..10000000 and timeout positive')
    if args.runner and (args.backend == 'qemu' or not args.runner.is_file() or
                        not os.access(args.runner, os.X_OK)):
        parser.error('--runner requires an executable PCEMU memory harness')
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    if args.backend != 'qemu' and not args.runner:
        subprocess.run(['make', '-s', 'build/memory-bench'], cwd=ROOT, check=True)
    program = build(work, args.loops)
    load_before = os.getloadavg()
    if args.backend == 'qemu':
        result = run_qemu(work, program, args.timeout)
        transcript = (work / 'guest.txt').read_text()
    else:
        runner = args.runner.resolve() if args.runner else ROOT / 'build/memory-bench'
        runner_hash = hashlib.sha256(runner.read_bytes()).hexdigest()
        command = [str(runner), str(program), str(work / 'raw.json'), '--backend', args.backend]
        subprocess.run(command, check=True, timeout=args.timeout)
        result = json.loads((work / 'raw.json').read_text())
        result['command'] = command
        if hashlib.sha256(runner.read_bytes()).hexdigest() != runner_hash:
            raise RuntimeError('Runner changed during measurement')
        result['runnerSha256'] = runner_hash
        transcript = (work / 'raw.json.guest.txt').read_text()
    records = validate(transcript, args.loops)
    if len(result['samples']) != len(records):
        raise ValueError('Timed interval count mismatch')
    for sample, record in zip(result['samples'], records):
        if sample['hostSeconds'] <= 0:
            raise ValueError('Non-positive interval')
        sample.update(record)
        sample['nanosecondsPerDependentLoad'] = sample['hostSeconds'] * 1e9 / record['dependentLoads']
        sample['shortInterval'] = sample['hostSeconds'] < .1
    result.update({'scope': __doc__, 'backend': args.backend, 'loops': args.loops,
                   'guestResultsValidated': True, 'guestTranscript': transcript,
                   'programSha256': hashlib.sha256(program.read_bytes()).hexdigest(),
                   'host': {'machine': platform.machine(), 'release': platform.release(),
                            'loadBefore': load_before, 'loadAfter': os.getloadavg()},
                   'sourceSha256': {str(f.relative_to(ROOT)): hashlib.sha256(f.read_bytes()).hexdigest()
                                    for f in [ROOT / 'tests/guest_perf/memory32.asm',
                                              ROOT / 'tests/guest_perf/dos_loader.asm',
                                              ROOT / 'tests/guest_perf/qemu_boot.asm',
                                              ROOT / 'tools/memory_bench.cpp',
                                              ROOT / 'tools/bench_guest_memory.py']}})
    # Only our tiny successful QEMU fixture; PCEMU opens no disk.
    disk = work / 'boot.raw'
    if disk.exists():
        disk.unlink()
    result['privateDiskRemoved'] = True
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS: {args.backend}, 20 checked memory intervals; {work}', flush=True)


if __name__ == '__main__':
    main()
