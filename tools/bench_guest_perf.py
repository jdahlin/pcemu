#!/usr/bin/env python3
"""Build and boot CoreMark in an original DOS/386 guest; retain validated host-timed results."""
import argparse,hashlib,json,os,platform,subprocess,tempfile
from pathlib import Path
from cow_file import CowFile
from fat_stage import Writer
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iterations',type=int,default=0,help='0: upstream automatic >=10s run; short fixed counts are diagnostic only')
p.add_argument('--no-jit',action='store_true')
p.add_argument('--timeout',type=int,default=180)
p.add_argument('--output',type=Path)
a=p.parse_args()
subprocess.run(['make','-s','build/guest-bench','build/disk-tool'],cwd=ROOT,check=True)
work=a.output.resolve() if a.output else Path(tempfile.mkdtemp(prefix='guest-perf-',dir=ROOT/'build'))
work.mkdir(parents=True,exist_ok=True)
subprocess.run(['python3',str(ROOT/'tools/build_guest_perf.py'),'--iterations',str(a.iterations),'--output',str(work/'program')],check=True)
disk=work/'disk.pcd'
if disk.exists():p.error('Destination disk already exists; choose a new output directory')
subprocess.run([str(ROOT/'build/disk-tool'),'overlay',str(disk),str(ROOT/'media/dos.raw')],check=True)
with CowFile(disk) as f:
    fat=Writer(f);program=(work/'program/PCBENCH.COM').read_bytes()
    fat.root_entry(fat.entry('PCBENCH.COM',32,fat.allocate(program),len(program)));fat.commit()
    fat.replace_existing('CONFIG  SYS',b'FILES=30\r\nBUFFERS=20\r\n')
    fat.replace_existing('AUTOEXECBAT',b'@ECHO OFF\r\nPCBENCH.COM\r\nECHO PCEMU benchmark returned to DOS\r\n')
    e=next(e for e in fat.entries() if e[1]=='PCBENCH COM')
    if fat.read_file(e[2],e[3])!=program:raise ValueError('Staged COM readback mismatch')
result=work/'result.json'
provenance={'runnerSha256':hashlib.sha256((ROOT/'build/guest-bench').read_bytes()).hexdigest(),
            'gitCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
            'host':{'system':platform.system(),'release':platform.release(),'machine':platform.machine(),'logicalCpus':os.cpu_count(),'loadAverageBefore':os.getloadavg()}}
command=[str(ROOT/'build/guest-bench'),'--disk',str(disk),'--output',str(result),'--timeout',str(a.timeout)]
if a.no_jit:command.append('--no-jit')
with (work/'run.log').open('w') as log:
    subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=a.timeout+10)
s=json.loads(result.read_text());s.update(provenance);s['host']['loadAverageAfter']=os.getloadavg();s['build']=json.loads((work/'program/build.json').read_text())
transcript=Path(str(result)+'.guest.txt').read_text()
import re
iterations=int(re.search(r'Iterations\s+: (\d+)',transcript).group(1))
s['iterations']=iterations;s['iterationsPerHostSecond']=iterations/s['samples'][-1]['hostSeconds']
s['publishedScoreEligible']=s['upstreamValidated'] and s['samples'][-1]['hostSeconds']>=10
result.write_text(json.dumps(s,indent=2)+'\n')
print(f"PASS: {s['iterationsPerHostSecond']:.3f} iterations/host second; CRC checked; returned to DOS; "
      +('>=10s upstream validation passed' if s['publishedScoreEligible'] else 'short diagnostic, not a publishable CoreMark score'))
print(f'Evidence: {work}')
disk.unlink() # Only this private compressed child; original media is unchanged.
