#!/usr/bin/env python3
"""Run pinned Intel 386EX hardware vectors with bounded parallel subprocesses."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import gzip
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import tempfile
from x86_moo import read_moo,write_vectors
ROOT=Path(__file__).resolve().parents[1]
VENDOR=ROOT/'tests/vendor/80386'


def fetch(manifest,name,sha,folder,metadata=False):
    """Verify pinned Git blob identities, including cached downloads."""
    path=folder/name
    if not path.exists():
        url=f"https://raw.githubusercontent.com/SingleStepTests/80386/{manifest['revision']}/"+('' if metadata else 'v1_ex_real_mode/')+name
        subprocess.run(['curl','--fail','--location','--silent','--show-error','--retry','2','--max-time','90',url,'-o',str(path)],check=True)
    data=path.read_bytes()
    digest=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
    if digest!=sha:raise ValueError('Pinned Git blob mismatch: '+name)
    return data


def opcode_row(opcode,rows):
    """Resolve size-prefixed and grouped filenames to their upstream metadata."""
    base,*extension=opcode.split('.')
    while base.startswith(('66','67')):base=base[2:]
    candidates=[r for r in rows if r['op']==base]
    if not extension and len(candidates)==1:return candidates[0]
    return next(r for r in candidates if r['ex']==(extension[0] if extension else ''))


def flag_mask(opcode,rows):
    """Use upstream per-opcode defined-flag masks, retaining 386 control flags."""
    row=opcode_row(opcode,rows)
    # Despite its historical name, f_umask contains ones for comparable flags.
    # E.g. MUL=FF2B retains CF/OF, excluding undefined SF/ZF/AF/PF.
    return 0x37fd7 & (0xffff0000|int(row['f_umask'],16)) if row['f_umask'] else 0x37fd7


def shift_contract(test, mask):
    """Mask only architecturally undefined flags; identify undefined double shifts.

    Intel 80386 PRM SAL/SHR/SAR and SHLD/SHRD: OF is defined only for
    count one, and double-shift results above operand width are undefined.
    https://pdos.csail.mit.edu/6.828/2018/readings/i386/SHLD.htm
    """
    code=test['bytes'];at=0;operand32=False
    while at<len(code) and code[at] in [0x26,0x2e,0x36,0x3e,0x64,0x65,0x66,0x67,0xf0,0xf2,0xf3]:
        operand32 |= code[at]==0x66;at+=1
    op=code[at];at+=1;double=False
    if op==0x0f:
        op=code[at];at+=1
        if op not in [0xa4,0xa5,0xac,0xad]:return mask,False
        double=True
    elif op not in [0xc0,0xc1,0xd0,0xd1,0xd2,0xd3]:return mask,False
    width=32 if operand32 else 16
    if not double and op in [0xc0,0xd0,0xd2]:width=8
    count=1 if op in [0xd0,0xd1] else (code[-2] if op in [0xc0,0xc1,0xa4,0xac] else test['initial']['regs']['ecx'])
    count &= 31
    if count==0:return 0x37fd7,False
    if count!=1:mask &= ~0x800
    group=(code[at]>>3)&7
    if not double and group in [4,5,6] and count>=width:mask &= ~1
    return mask,double and count>width and 'exception' not in test


def undefined_sib(test):
    """Exclude the upstream-documented 386EX scaled-no-index SIB quirk."""
    code=test['bytes'];at=0;address32=False
    while at<len(code) and code[at] in [0x26,0x2e,0x36,0x3e,0x64,0x65,0x66,0x67,0xf0,0xf2,0xf3]:
        address32 |= code[at]==0x67;at+=1
    if not address32 or at>=len(code):return False
    at+=2 if code[at]==0x0f else 1
    if at+1>=len(code):return False
    m=code[at];sib=code[at+1]
    return m>>6!=3 and m&7==4 and (sib>>3)&7==4 and sib>>6!=0


def run(args):
    manifest=json.loads((VENDOR/'UPSTREAM.json').read_text())
    work=Path(tempfile.mkdtemp(prefix='x86-hardware-',dir=ROOT/'build'))
    runner=work/'runner';shutil.copyfile(ROOT/'build/x86-external-runner',runner);runner.chmod(0o755)
    report={'suite':'SingleStepTests/80386','revision':manifest['revision'],'backend':args.backend,'jobs':1 if args.quick else args.jobs,'results':[],
            'runnerSHA256':hashlib.sha256(runner.read_bytes()).hexdigest(),'status':'running'}
    if args.quick:
        data=(VENDOR/'quick.json').read_bytes()
        if hashlib.sha256(data).hexdigest()!=manifest['quickSHA256']:raise ValueError('Vendored sample checksum mismatch')
        tests=json.loads(data)['tests'];path=work/'quick.bin';write_vectors(path,tests)
        result=subprocess.run([str(runner),'vectors',str(path),args.backend],capture_output=True,text=True)
        (work/'quick.log').write_text(result.stdout+result.stderr)
        report['results']=[{'case':'offline pinned sample','vectors':len(tests),'returncode':result.returncode}]
    else:
        cache=ROOT/'build/x86-external/80386';cache.mkdir(parents=True,exist_ok=True)
        for name,sha in manifest['metadata'].items():fetch(manifest,name,sha,cache,True)
        with (cache/'80386.csv').open() as f:rows=list(csv.DictReader(f))
        revoked=set((cache/'revocation_list.txt').read_text().split())
        opcodes=args.opcodes.split(',') if args.opcodes else manifest['defaultOpcodes']
        if opcodes==['all']:opcodes=[name.removesuffix('.MOO.gz') for name in manifest['files']]
        if len(set(opcodes))!=len(opcodes):raise ValueError('Duplicate opcode shard')
        def shard(opcode):
            name=opcode+'.MOO.gz'
            if name not in manifest['files']:raise ValueError('Unknown opcode file: '+name)
            data=fetch(manifest,name,manifest['files'][name],cache)
            tests=read_moo(gzip.decompress(data),capture_io=opcode_row(opcode,rows)['io']=='1');selected=[];revoked_count=0;undefined_count=0;undefined_shift=0
            for test in tests:
                if test['hash'] in revoked:revoked_count+=1;continue
                if opcode_row(opcode,rows)['m']=='1' and undefined_sib(test):undefined_count+=1;continue
                mask,undefined=shift_contract(test,flag_mask(opcode,rows))
                if undefined:undefined_shift+=1;continue
                test.update(opcode=opcode,flagMask=mask);selected.append(test)
            if args.index is not None:selected=[t for t in selected if t['idx']==args.index]
            if args.limit:selected=selected[:args.limit]
            if not selected:raise ValueError('No applicable vectors for '+opcode)
            path=work/(opcode+'.bin');write_vectors(path,selected)
            result=subprocess.run([str(runner),'vectors',str(path),args.backend],capture_output=True,text=True,timeout=180)
            (work/(opcode+'.log')).write_text(result.stdout+result.stderr);path.unlink()
            record={'opcode':opcode,'sourceVectors':len(tests),'revoked':revoked_count,'undefinedSib':undefined_count,'undefinedDoubleShift':undefined_shift,'vectors':len(selected),
                    'fileSHA256':hashlib.sha256(data).hexdigest(),'returncode':result.returncode,'summary':result.stdout.splitlines()[-1:]}
            print(opcode,record['summary'],flush=True);return record
        # Threads coordinate I/O; CPU execution happens in bounded native child processes.
        def safe_shard(opcode):
            try:return shard(opcode)
            except Exception as error:
                return {'opcode':opcode,'returncode':2,'error':str(error),'vectors':0}
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for result in pool.map(safe_shard,opcodes):report['results'].append(result)
    report['status']='passed' if all(r['returncode']==0 for r in report['results']) else 'failed'
    if report['status']=='passed' and any(r.get('unsupported') for r in report['results']):report['status']='incomplete'
    report['scope']='architectural registers, defined flags, exception identity, listed RAM and captured port bytes; no timing, within-operation bus-lane order or unlisted-write oracle'
    (work/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    runner.unlink()
    print('Evidence:',work,flush=True)
    if report['status']!='passed':raise SystemExit(1)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--quick',action='store_true',help='small vendored sample; offline')
    p.add_argument('--backend',choices=['interpreter','jit','both'],default='both')
    p.add_argument('--jobs',type=int,default=min(4,os.cpu_count() or 1))
    p.add_argument('--opcodes',help='comma-separated shards, or all for the entire corpus')
    p.add_argument('--index',type=int,help='reproduce one upstream vector index per selected shard')
    p.add_argument('--limit',type=int,default=0,help='maximum vectors per shard; zero runs all')
    args=p.parse_args()
    if not 1<=args.jobs<=32 or args.limit<0 or (args.index is not None and args.index<0):p.error('jobs must be 1..32 and index/limit nonnegative')
    run(args)
