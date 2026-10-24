#!/usr/bin/env python3
"""Compare synthetic arithmetic or RAM loops on ARM64, Rosetta, and PCEmu.
This excludes system calls, devices, and application startup. Native memory
kernels omit guest MMU semantics and add a counter instruction per loop.
"""
import argparse
import hashlib
import json
import platform
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--blocks', type=int, default=5000000)
p.add_argument('--memory', action='store_true', help='Compare sixteen load/add/store sequences')
p.add_argument('--emulator', type=Path, help='use a frozen executable without rebuilding it')
p.add_argument('--output', type=Path, help='retain this comparison without overwriting older evidence')
a = p.parse_args()
if platform.system() != 'Darwin' or platform.machine() != 'arm64':
    p.error('Requires an Apple Silicon Mac with Rosetta installed')
if not 0 < a.blocks <= 0xffffffff // 48:
    p.error('Block count must fit the 32-bit accumulator')
subprocess.run(['pkgutil', '--pkg-info', 'com.apple.pkg.RosettaUpdateAuto'],
               check=True, stdout=subprocess.DEVNULL)
if not a.emulator:
    subprocess.run(['make', '-s'], cwd=root, check=True)
emulator = a.emulator.resolve() if a.emulator else root / 'build/pcemu'
if not emulator.is_file():
    p.error('Missing emulator executable')
emulator_hash = hashlib.sha256(emulator.read_bytes()).hexdigest()
kind = 'memory' if a.memory else 'alu'
results = {'emulatorPath':str(emulator), 'emulatorSha256':emulator_hash, 'timestamp': datetime.now(timezone.utc).isoformat(), 'blocks': a.blocks,
           'scope': ('memory microbenchmark; native kernels add a loop counter instruction; '
                     'no guest MMU/device semantics; not application performance' if a.memory else
                     'register-only arithmetic microbenchmark; not application performance')}
for arch in ['arm64', 'x86_64']:
    binary = root / f'build/bench-{kind}-{arch}'
    subprocess.run(['clang++', '-O3', '-arch', arch, str(root / f'tests/bench_{kind}.cpp'),
                    str(root / f'tests/bench_{kind}_{arch}.s'), '-o', str(binary)], check=True)
    command = [str(binary), str(a.blocks)]
    if arch == 'x86_64':
        command = ['arch', '-x86_64', *command]
    results[arch] = subprocess.check_output(command, text=True).strip()
    print(f'{arch}: {results[arch]}', flush=True)
results['pcemu'] = subprocess.check_output([
    str(emulator), '--headless', '--benchmark-memory' if a.memory else '--benchmark', '--benchmark-steps',
    str(a.blocks * (49 if a.memory else 50))
], cwd=root, text=True).strip()
expected = a.blocks * (16 if a.memory else 48)
checksums = re.findall(r'checksum=(\d+)' if a.memory else r'EAX=(\d+)', results['pcemu'])
if len(checksums) != (4 if a.memory else 2) or any(int(value) != expected for value in checksums):
    raise RuntimeError('PCEmu result does not match the native kernel checksum')
results['checksumsValidated'] = True
print(results['pcemu'])
if hashlib.sha256(emulator.read_bytes()).hexdigest() != emulator_hash:
    raise RuntimeError('Emulator changed during comparison')
output = a.output or root / ('build/rosetta-memory-benchmark.json' if a.memory else 'build/rosetta-register-benchmark.json')
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(results, indent=2) + '\n')
