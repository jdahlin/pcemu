#!/usr/bin/env python3
"""Build unchanged pinned CoreMark for the original minimal DOS/386 loader."""
import argparse, hashlib, json, shutil, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iterations',type=int,default=0,help='0: upstream automatic >=10s run; fixed short runs are diagnostics')
p.add_argument('--output',type=Path,default=ROOT/'build/guest-perf')
a=p.parse_args()
if not 0<=a.iterations<=0x7fffffff:p.error('iterations outside signed 32-bit range')
a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
vendor=ROOT/'tests/vendor/coremark';port=ROOT/'tests/guest_perf'
manifest=json.loads((vendor/'UPSTREAM.json').read_text())
for name,digest in manifest['files'].items():
    if hashlib.sha256((vendor/name).read_bytes()).hexdigest()!=digest:raise ValueError('Modified upstream source: '+name)
compiler=shutil.which('clang')
linker=shutil.which('ld.lld') or '/opt/homebrew/opt/llvm@17/bin/ld.lld'
flags=['--target=i386-none-elf','-march=i386','-mno-sse','-mno-mmx','-msoft-float','-O2','-ffreestanding','-fno-builtin','-fno-pic','-fno-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables',f'-DITERATIONS={a.iterations}',f'-I{port}',f'-I{vendor}']
objects=[]
for source in [*sorted(vendor.glob('*.c')),port/'core_portme.c']:
    obj=a.output/(source.stem+'.o');subprocess.run([compiler,*flags,'-c',str(source),'-o',str(obj)],check=True);objects.append(str(obj))
entry=a.output/'entry.o';subprocess.run(['nasm','-f','elf32',str(port/'entry.asm'),'-o',str(entry)],check=True)
payload=a.output/'payload.bin'
subprocess.run([linker,'-m','elf_i386','--oformat=binary','-T',str(port/'payload.ld'),str(entry),*objects,'-o',str(payload)],check=True)
program=a.output/'PCBENCH.COM'
subprocess.run(['nasm','-f','bin',str(port/'dos_loader.asm'),f'-DPAYLOAD="{payload}"','-o',str(program)],check=True)
if program.stat().st_size>65000:raise ValueError('DOS COM exceeds one segment')
metadata={'upstream':manifest['upstream'],'commit':manifest['commit'],'compiler':subprocess.check_output([compiler,'--version'],text=True).splitlines()[0],'flags':flags,'iterations':a.iterations,'comBytes':program.stat().st_size,'comSha256':hashlib.sha256(program.read_bytes()).hexdigest()}
(a.output/'build.json').write_text(json.dumps(metadata,indent=2)+'\n')
print(f'Built {program}: {program.stat().st_size} bytes')
