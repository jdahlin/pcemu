#!/usr/bin/env python3
"""Build pinned CoreMark or original x86 microkernels for the minimal DOS/386 loader."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--suite', choices=['coremark', 'micro'], default='coremark')
parser.add_argument('--iterations', type=int, default=0,
                    help='0: upstream automatic >=10s run; fixed short runs are diagnostics')
parser.add_argument('--output', type=Path, default=ROOT / 'build/guest-perf')
args = parser.parse_args()
if not 0 <= args.iterations <= 0x7fffffff:
    parser.error('iterations outside signed 32-bit range')
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
vendor = ROOT / 'tests/vendor/coremark'
port = ROOT / 'tests/guest_perf'
manifest = json.loads((vendor / 'UPSTREAM.json').read_text())
for name, digest in manifest['files'].items():
    if hashlib.sha256((vendor / name).read_bytes()).hexdigest() != digest:
        raise ValueError('Modified upstream source: ' + name)
compiler = shutil.which('clang')
nasm = shutil.which('nasm')
linker = shutil.which('ld.lld') or '/opt/homebrew/opt/llvm@17/bin/ld.lld'
if not compiler or not nasm or not Path(linker).is_file():
    parser.error('Clang, NASM and an ELF-capable ld.lld are required')
flags = ['--target=i386-none-elf', '-march=i386', '-mno-sse', '-mno-mmx',
         '-msoft-float', '-O2', '-ffreestanding', '-fno-builtin', '-fno-pic',
         '-fno-pie', '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
         f'-DITERATIONS={args.iterations}', f'-I{port}', f'-I{vendor}']
if args.suite == 'micro':
    flags.append('-DPCEMU_MICRO_ONLY=1')
objects = []
for source in [*sorted(vendor.glob('*.c')), port / 'core_portme.c']:
    obj = output / (source.stem + '.o')
    subprocess.run([compiler, *flags, '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
entry = output / 'entry.o'
subprocess.run([nasm, '-f', 'elf32', str(port / 'entry.asm'), '-o', str(entry)], check=True)
if args.suite == 'micro':
    micro = output / 'micro32.o'
    subprocess.run([nasm, '-f', 'elf32', str(port / 'micro32.asm'), '-o', str(micro)], check=True)
    objects.append(str(micro))
payload = output / 'payload.bin'
subprocess.run([linker, '-m', 'elf_i386', '--oformat=binary', '-T', str(port / 'payload.ld'),
                str(entry), *objects, '-o', str(payload)], check=True)
program = output / 'PCBENCH.COM'
subprocess.run([nasm, '-f', 'bin', '-I' + str(port) + '/', str(port / 'dos_loader.asm'),
                f'-DPAYLOAD="{payload}"',
                *(['-DMICRO_SUITE=1'] if args.suite == 'micro' else []),
                '-o', str(program)], check=True)
if program.stat().st_size > 65000:
    raise ValueError('DOS COM exceeds one segment')
metadata = {
    'suite': args.suite, 'upstream': manifest['upstream'], 'commit': manifest['commit'],
    'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
    'flags': flags, 'iterations': args.iterations, 'comBytes': program.stat().st_size,
    'comSha256': hashlib.sha256(program.read_bytes()).hexdigest(),
}
(output / 'build.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(f'Built {program}: {program.stat().st_size} bytes')
