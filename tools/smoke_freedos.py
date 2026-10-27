#!/usr/bin/env python3
"""Headless FreeDOS boot, file/ATA, IRQ and real guest-driver acceptance checks."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from cow_file import CowFile
from fat_stage import Writer
from freedos_fixture import DEFAULT, prepare, read_path, startup, put_root

ROOT=Path(__file__).resolve().parents[1]
PROFILES={
 'hma':([],b'DEVICE=C:\\FREEDOS\\BIN\\HIMEMX.EXE\r\nDOS=HIGH\r\n',b''),
 'minimal':([],b'',b''),
 'xms':(['XMS','MOUSE'],b'DEVICE=C:\\FREEDOS\\BIN\\HIMEMX.EXE\r\nDOS=HIGH\r\n',b'C:\\DRIVERS\\CTMOUSE.EXE /P\r\n'),
 'ems':(['XMS','EMS','MOUSE'],b'DEVICE=C:\\FREEDOS\\BIN\\JEMMEX.EXE I=E000-EFFF FRAME=E000 X=C000-DFFF X=F000-FFFF\r\nDOS=HIGH,UMB\r\n',b'C:\\DRIVERS\\CTMOUSE.EXE /P\r\n'),
 'network':([],b'',b'C:\\DRIVERS\\NE2000.COM 0x60 0xA 0x300\r\nSET MTCPCFG=C:\\TCP.CFG\r\nC:\\DRIVERS\\DHCP.EXE > C:\\DHCP.LOG\r\nIF ERRORLEVEL 1 GOTO FAILED\r\n'),
}


def screen(control,work,process):
    """Read paused character-plane text, leaving VGA latches untouched."""
    path=work/'vga.json';old=path.stat().st_mtime_ns if path.exists() else 0
    with control.open('a') as f:f.write('pause\ndump-vga '+str(path)+'\n')
    end=time.monotonic()+10
    while time.monotonic()<end:
        if process.poll() is not None:return ''
        try:
            if path.stat().st_mtime_ns>old:
                state=json.loads(path.read_text());break
        except (FileNotFoundError,json.JSONDecodeError):pass
        time.sleep(.01)
    else:raise TimeoutError('No VGA diagnostic state')
    chars=bytes.fromhex(state['planes'][0])[:4000]
    text='\n'.join(''.join(chr(c) if 32<=c<127 else ' ' for c in chars[i:i+80]) for i in range(0,len(chars),80))
    return text


def run_profile(args,parent,profile):
    """Run an isolated profile, require exact probe records and remove its child."""
    work=Path(tempfile.mkdtemp(prefix='freedos-'+profile+'-',dir=ROOT/'build'))
    disk=work/'disk.qcow2';control=work/'control.txt';success=False
    flags,config,drivers=PROFILES[profile]
    program=work/'CHECK.COM'
    subprocess.run(['nasm','-f','bin',str(ROOT/('tests/freedos/dosemu_hma.asm' if profile=='hma' else 'tests/freedos/drivers.asm')),*[f'-DEXPECT_{f}=1' for f in flags],'-o',str(program)],check=True)
    subprocess.run([str(ROOT/'build/disk-tool'),'overlay',str(disk),str(parent)],check=True)
    with CowFile(disk) as handle:
        fat=Writer(handle)
        put_root(fat,'CHECK.COM',program.read_bytes())
        if profile=='network':put_root(fat,'TCP.CFG',b'PACKETINT 0x60\r\nHOSTNAME PCEMU\r\n')
        startup(fat,b'FILES=40\r\nBUFFERS=20\r\n'+config,
                b'@ECHO OFF\r\nPATH C:\\FREEDOS\\BIN;C:\\DRIVERS\r\n'+drivers+
                b'C:\\CHECK.COM > C:\\PROBE.LOG\r\nIF ERRORLEVEL 1 GOTO FAILED\r\n'
                b'ECHO PCEMU VALIDATION DONE\r\nGOTO END\r\n:FAILED\r\nECHO PCEMU VALIDATION FAILED\r\n:END\r\n')
        fat.commit()
    runner=work/'runner';shutil.copyfile(ROOT/'build/pcemu',runner);runner.chmod(0o755)
    rom=work/'vga.bin';shutil.copyfile(ROOT/'build/firmware/pcemu-vga.bin',rom)
    command=[str(runner),'--headless','--disk',str(disk),'--vgabios',str(rom),'--steps','0',
             '--control-file',str(control),'--until-text','PCEMU VALIDATION','--state-json',str(work/'state.json'),'--frame',str(work/'frame.ppm')]
    if profile!='network':command+=['--no-network']
    if args.no_jit:command+=['--no-jit']
    record={'profile':profile,'backend':'interpreter' if args.no_jit else 'jit',
            'runnerSHA256':hashlib.sha256(runner.read_bytes()).hexdigest(),
            'romSHA256':hashlib.sha256(rom.read_bytes()).hexdigest(),
            'probeSHA256':hashlib.sha256(program.read_bytes()).hexdigest(),
            'parentSHA256':hashlib.sha256(parent.read_bytes()).hexdigest(),
            'sourceCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
            'config':config.decode('ascii'),'driverCommands':drivers.decode('ascii'),'status':'running'}
    process=None
    try:
        with (work/'guest.log').open('w') as log:
            process=subprocess.Popen(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
            end=time.monotonic()+args.timeout;motion=False
            while process.poll() is None:
                if time.monotonic()>end:raise TimeoutError('FreeDOS '+profile+' timed out')
                if 'MOUSE' in flags and not motion:
                    text=screen(control,work,process)
                    with control.open('a') as f:
                        f.write('resume\n')
                        if 'PCEMU MOUSE WAIT' in text:
                            f.write('mouse 16 -8 0\n');f.flush();time.sleep(.2)
                            f.write('key 3920\n');motion=True
                    if 'PCEMU VALIDATION FAILED' in text:raise RuntimeError('Guest reported failure: '+text)
                time.sleep(.1)
            if process.returncode:raise RuntimeError('Emulator exited with '+str(process.returncode))
        with CowFile(disk) as handle:
            fat=Writer(handle);text=read_path(fat,'PROBE.LOG').decode('cp437')
            (work/'probe.log').write_text(text)
            expected=['DOS-FILE-8192','ATA-PIO-IDENTIFY-READ','BIOS-KEYBOARD-RING','PIT-IRQ-TICKS']
            if 'XMS' in flags:expected+=['XMS-MOVE-8192']
            if 'EMS' in flags:expected+=['EMS-4-PAGES-65536']
            if 'MOUSE' in flags:expected+=['PS2-MOUSE-IRQ-MOTION']
            if profile=='hma':expected=['HMA-AVAILABLE','HMA-ALLOC-35-FREEDOS']
            expected+=['PROBE-COMPLETE']
            if text.splitlines()!=['PASS '+item for item in expected]:raise RuntimeError('Unexpected probe results: '+text)
            if profile=='network':
                dhcp=read_path(fat,'DHCP.LOG').decode('cp437');(work/'dhcp.log').write_text(dhcp)
                settings=read_path(fat,'TCP.CFG').decode('cp437')
                if 'Offer received, Acknowledged' not in dhcp or 'IPADDR 10.0.2.15' not in settings or 'GATEWAY 10.0.2.2' not in settings:
                    raise RuntimeError('DHCP lease/committed configuration not verified: '+dhcp)
                record['dhcp']=dhcp
        if hashlib.sha256(parent.read_bytes()).hexdigest()!=record['parentSHA256']:
            raise RuntimeError('Read-only fixture parent changed')
        record.update(status='passed',checks=expected);success=True
        print('PASS',profile,record['backend'],work,flush=True)
    except BaseException as error:
        record.update(status='failed',error=str(error));raise
    finally:
        if process and process.poll() is None:
            with control.open('a') as f:f.write('quit\n')
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=10)
        (work/'report.json').write_text(json.dumps(record,indent=2)+'\n')
        if success:
            for path in [disk,runner,rom,work/'vga.json']:path.unlink(missing_ok=True)
        print('Evidence:',work,flush=True)
    return record


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk',type=Path,help='existing generated FreeDOS fixture; otherwise prepare it')
    parser.add_argument('--profile',choices=['all',*PROFILES],default='all')
    parser.add_argument('--no-jit',action='store_true')
    parser.add_argument('--timeout',type=float,default=90)
    args=parser.parse_args()
    parent=args.disk.resolve() if args.disk else prepare()
    for profile in ['minimal','xms','ems','network'] if args.profile=='all' else [args.profile]:run_profile(args,parent,profile)


if __name__=='__main__':main()
