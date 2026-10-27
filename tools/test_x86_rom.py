#!/usr/bin/env python3
"""Run pinned test386.asm ROM checks separately from the quick offline suite."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path,PurePosixPath
import shutil
import subprocess
import tempfile
import time
import zipfile
ROOT=Path(__file__).resolve().parents[1]
REVISION='cfd052d1e64d5375dea5a681c1eadeed64ceda2c'
SHA256='6da205bb4364adf0b357b73a02c045b0b5b58e4a606530b74c9f1672164749db'
URL=f'https://codeload.github.com/barotto/test386.asm/zip/{REVISION}'


def prepare(work):
    """Verify upstream sources; change only its documented ASCII output port."""
    cache=ROOT/'build/x86-external';cache.mkdir(parents=True,exist_ok=True);archive=cache/'test386.zip'
    if not archive.exists():subprocess.run(['curl','--fail','--location','--retry','2','--max-time','90',URL,'-o',str(archive)],check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=SHA256:raise ValueError('test386 source checksum mismatch')
    with zipfile.ZipFile(archive) as z:
        prefix='test386.asm-'+REVISION+'/'
        for name in z.namelist():
            relative=name.removeprefix(prefix)
            if name.endswith('/') or not (relative.startswith('src/') or relative in ['COPYING','README.md']):continue
            if name==relative or '..' in PurePosixPath(relative).parts:raise ValueError('Unexpected source archive path')
            path=work/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(z.read(name))
    config=work/'src/configuration.asm';source=config.read_text()
    if source.count('OUT_PORT equ 0\n')!=1:raise ValueError('Unexpected upstream port configuration')
    config.write_text(source.replace('OUT_PORT equ 0\n','OUT_PORT equ 0xe9\n'))
    rom=work/'test386.bin'
    with (work/'assemble.log').open('w') as log:
        subprocess.run(['nasm','-i',str(work/'src')+'/', '-f','bin',str(work/'src/test386.asm'),'-o',str(rom)],stdout=log,stderr=subprocess.STDOUT,check=True)
    return rom


def qemu(rom,work):
    """Use an independent CPU model; require final POST FF, then stop the owned VM."""
    output=work/'qemu.txt';posts=work/'qemu.post';log=(work/'qemu.log').open('w')
    command=['qemu-system-i386','-machine','pc','-cpu','486','-accel','tcg','-m','16','-bios',str(rom),
             '-display','none','-serial','none','-monitor','none','-no-reboot',
             '-chardev',f'file,id=post,path={posts}','-device','isa-debugcon,iobase=0x190,chardev=post',
             '-chardev',f'file,id=result,path={output}','-device','isa-debugcon,iobase=0xe9,chardev=result']
    process=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT);success=False
    try:
        end=time.monotonic()+120
        while time.monotonic()<end and process.poll() is None:
            if posts.exists() and posts.read_bytes().endswith(b'\xff'):success=True;break
            time.sleep(.1)
    finally:
        if process.poll() is None:
            process.terminate()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        log.close()
    return {'backend':'qemu-486-tcg','returncode':0 if success else 1,'posts':list(posts.read_bytes()) if posts.exists() else []}


def run(args):
    work=Path(tempfile.mkdtemp(prefix='x86-test386-',dir=ROOT/'build'));rom=prepare(work)
    frozen=work/'runner';shutil.copyfile(ROOT/'build/x86-external-runner',frozen);frozen.chmod(0o755)
    report={'suite':'barotto/test386.asm','jobs':args.jobs,'revision':REVISION,'archiveSHA256':SHA256,'configuration':'OUT_PORT=E9; remaining upstream defaults',
            'romSHA256':hashlib.sha256(rom.read_bytes()).hexdigest(),'runnerSHA256':hashlib.sha256(frozen.read_bytes()).hexdigest(),'results':[]}
    def native(backend):
        p=subprocess.run([str(frozen),'rom',str(rom),backend,str(work/(backend+'.txt'))],capture_output=True,text=True,timeout=130)
        (work/(backend+'.log')).write_text(p.stdout+p.stderr)
        return {'backend':backend,'returncode':p.returncode,'summary':p.stdout.splitlines()[-1:]}
    backends=['interpreter','jit']+(['qemu'] if args.qemu else [])
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(lambda b:qemu(rom,work) if b=='qemu' else native(b),backends):
            report['results'].append(result);print(result,flush=True)
    report['status']='passed' if all(r['returncode']==0 for r in report['results']) else 'failed'
    if args.qemu and report['results'][-1]['returncode']:
        report['status']='reference-inconclusive'
        report['note']='QEMU did not reach POST FF either; this ROM/configuration is not a validated full-suite oracle.'
    if args.qemu and report['results'][-1]['returncode']==0:
        reference=(work/'qemu.txt').read_bytes()
        for result in report['results'][:-1]:
            if result['returncode']==0:
                actual=(work/(result['backend']+'.txt')).read_bytes()
                result['sameArithmeticTranscriptAsQemu']=actual==reference
                if actual!=reference:report['status']='failed'
    (work/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    frozen.unlink();print('Evidence:',work,flush=True)
    if report['status']!='passed':raise SystemExit(1)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--jobs',type=int,default=2);p.add_argument('--qemu',action='store_true')
    args=p.parse_args()
    if not 1<=args.jobs<=32:p.error('jobs must be 1..32')
    run(args)
