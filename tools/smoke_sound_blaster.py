#!/usr/bin/env python3
"""Headless RAM-only DSP/DMA/IRQ probe; optional unchanged QEMU SB16 execution."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu',action='store_true')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='sb-smoke-',dir=ROOT/'build') as folder:
        disk=Path(folder)/'probe.img'
        subprocess.run(['nasm','-Wall','-Werror','-w-reloc-abs-word','-w-reloc-abs-dword','-f','bin',str(ROOT/'tests/sound-blaster-boot.asm'),'-o',str(disk)],check=True)
        for backend in ['interpreter','jit']:
            result=subprocess.run([str(ROOT/'build/pcemu'),'--headless','--backend',backend,'--no-network',
                '--floppy',str(disk),'--boot-floppy','--steps','2000000','--until-text','PCEMU-SB-PASS',
                '--state-json',str(Path(folder)/'state.json')],capture_output=True,text=True,timeout=30)
            if result.returncode or 'PCEMU-SB-PASS' not in result.stdout or 'PCEMU-SB-FAIL' in result.stdout:
                raise RuntimeError(backend+' guest failed:\n'+result.stdout+result.stderr)
            print('PASS Sound Blaster guest',backend,flush=True)
        if args.qemu:
            qemu=shutil.which('qemu-system-i386')
            if not qemu:parser.error('qemu-system-i386 is required')
            result=subprocess.run([qemu,'-machine','pc','-accel','tcg','-m','16',
                '-audiodev','driver=none,id=silent','-device','sb16,iobase=0x220,irq=7,dma=1,audiodev=silent',
                '-drive',f'file={disk},format=raw,if=floppy,readonly=on','-boot','a',
                '-display','none','-serial','none','-monitor','none','-no-reboot','-debugcon','stdio',
                '-device','isa-debug-exit,iobase=0xf4,iosize=0x04'],capture_output=True,text=True,timeout=30)
            if result.returncode!=33 or result.stdout!='PCEMU-SB-PASS':
                raise RuntimeError(f'QEMU guest failed ({result.returncode}):\n'+result.stdout+result.stderr)
            print('PASS same Sound Blaster guest on QEMU SB16',flush=True)

if __name__=='__main__':main()
