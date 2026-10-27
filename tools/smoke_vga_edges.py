#!/usr/bin/env python3
"""Headless external VGA edge cases, compared against the same ROM under QEMU."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from PIL import ImageChops
from cow_file import CowFile
from fat_stage import Writer
from prepare_media import FAT
from fetch_vga_diagnostics import prepare, prepare_edges
from vga_text_oracle import render_text
from smoke_vga_diagnostics import PcGuest, QemuGuest, normalize, ocr, pixels

ROOT = Path(__file__).resolve().parents[1]


def key(guest, name):
    """Deliver a BIOS scan code or equivalent QEMU virtual keyboard key."""
    codes = {'right':'4d00','left':'4b00','up':'4800','down':'5000',
             'pgup':'4900','pgdn':'5100','plus':'002b','minus':'0c2d', 'esc':'011b', 'ret':'1c0d', 'space':'3920', 'f8':'4200', 'f9':'4300', 'f10':'4400', 'home':'4700'}
    if isinstance(guest, PcGuest):
        guest.send('resume')
        guest.send('key ' + codes[name])
    else:
        guest.command_qmp('cont')
        guest.command_qmp('human-monitor-command', {'command-line':'sendkey '+{'plus':'shift-equal','minus':'minus','space':'spc'}.get(name,name)})
    time.sleep(.2)


class EdgeRun:
    """Own small compressed child disks and frozen firmware/executable inputs."""
    def __init__(self, args):
        self.args = args
        self.work = Path(tempfile.mkdtemp(prefix='vga-edge-', dir=ROOT/'build'))
        self.rom, self.runner = self.work/'rom.bin', self.work/'runner'
        shutil.copyfile(args.rom,self.rom)
        shutil.copyfile(ROOT/'build/pcemu',self.runner);self.runner.chmod(0o755)
        self.guests=[];self.disks=[];self.at_vgatest=True
        self.report={'romSHA256':hashlib.sha256(self.rom.read_bytes()).hexdigest(),
                     'runnerSHA256':hashlib.sha256(self.runner.read_bytes()).hexdigest(),
                     'harnessSHA256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                     'sourceCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                     'qemuClock':'icount shift=7,align=off,sleep=off; functional checks only',
                     'backend':'interpreter' if args.no_jit else 'jit','results':[], 'status':'running'}

    def overlay(self, name, parent):
        """Create only a small owned overlay; never modify its existing parent."""
        path=self.work/(name+'.qcow2')
        subprocess.run([str(ROOT/'build/disk-tool'),'overlay',str(path),str(parent.resolve())],check=True)
        self.disks.append(path)
        return path

    def start(self):
        """Boot unmodified VGATEST on independent PCEmu and QEMU children."""
        folder=ROOT/'build/vga-diagnostics/tools';self.report['diagnostics']=prepare(folder)
        self.report['edgeDiagnostics']=prepare_edges(ROOT/'build/vga-edge/tools')
        seed=self.overlay('seed',self.args.disk)
        with CowFile(seed) as disk:
            fat=Writer(disk)
            fat.replace_existing('CONFIG  SYS',b'FILES=40\r\nBUFFERS=20\r\n')
            fat.replace_existing('AUTOEXECBAT',b'@ECHO OFF\r\nPATH C:\\DOS\r\nCD \\DIAG\r\nVGATEST0.EXE\r\n')
            files={'VGATEST0.EXE':(folder/'VGATEST0.EXE').read_bytes()}
            files.update({p.name:p.read_bytes() for p in (ROOT/'build/vga-edge/tools').glob('*.EXE')})
            files['X-VESA.COM']=(folder/'X-VESA.COM').read_bytes()
            files['RUNDT.BAT']=b'@ECHO OFF\r\nDTEST.EXE\r\nECHO DOSLIB-DONE\r\n'
            fat.put_directory('DIAG',files)
            disk.flush()
        for kind in ['pcemu','qemu']:
            path=self.overlay(kind,seed)
            guest=PcGuest(self.work/kind,path,self.rom,self.runner,self.args.no_jit) if kind=='pcemu' else QemuGuest(self.work/kind,path,self.rom,icount=True)
            self.guests.append(guest)
            for _ in range(60):
                guest.text('')
                image=guest.capture()
                if 'which mode' in ocr(image,guest.work/'ready.png'):break
            else:raise RuntimeError(kind+': no VGATEST menu')

    def shell(self):
        """Leave the initial diagnostic menu exactly once."""
        if self.at_vgatest:
            self.text('q');self.at_vgatest=False

    def text(self,value):
        for guest in self.guests:guest.text(value)

    def key(self,name,count=1):
        for _ in range(count):
            for guest in self.guests:key(guest,name)

    def compare(self,name,columns=None,geometry=None,rows=25):
        """Compare every normalized pixel; retry only bounded drawing/blink phases."""
        for attempt in range(40):
            images=[normalize(g.capture(),columns,geometry,rows) for g in self.guests]
            different=sum(a!=b for a,b in zip(pixels(images[0]),pixels(images[1])))
            if images[0].size!=images[1].size:raise RuntimeError('capture geometry mismatch')
            if not different and len(set(pixels(images[0])))>=2:break
            for guest in self.guests:guest.text('')
        record={'case':name,'dimensions':images[0].size,'differentPixels':different,
                'pcemuSHA256':hashlib.sha256(images[0].tobytes()).hexdigest(),
                'qemuSHA256':hashlib.sha256(images[1].tobytes()).hexdigest()}
        self.report['results'].append(record)
        for guest,image in zip(self.guests,images):
            image.point(lambda c:(c<<2)|(c>>4)).save(guest.work/(name+'.png'))
        if different:
            ImageChops.difference(*images).point(lambda c:min(255,c*4)).save(self.work/(name+'-diff.png'))
            raise RuntimeError(f'{name}: {different} pixels differ')
        print(name+': exact QEMU comparison passed',flush=True)

    def device_state(self,guest):
        """Read paused native device state without changing VGA read latches."""
        dump=guest.work/'vga.json';old=dump.stat().st_mtime_ns if dump.exists() else 0
        guest.send('dump-vga '+str(dump))
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            try:
                if dump.stat().st_mtime_ns>old:
                    state=json.loads(dump.read_text());break
            except (FileNotFoundError,json.JSONDecodeError):pass
            time.sleep(.02)
        else:raise TimeoutError('No VGA diagnostic dump')
        return state

    def read_text(self,guest):
        """Read diagnostic ASCII cells directly; no OCR guesses affect pass/fail."""
        if isinstance(guest,PcGuest):
            state=self.device_state(guest)
            chars=bytes.fromhex(state['planes'][0])
        else:
            # Hardware debugger reads text only between tests. A subsequent mode
            # set reinitializes VGA before framebuffer reliability operations.
            path=guest.work/'text.bin'
            answer=guest.command_qmp('human-monitor-command',{'command-line':f'pmemsave 0xb8000 0x8000 "{path}"'})
            if answer:raise RuntimeError('QEMU text read failed: '+answer)
            chars=path.read_bytes()[::2]
        return '\n'.join(''.join(chr(c) if 32<=c<127 else ' ' for c in chars[n:n+80]) for n in range(0,min(len(chars),8000),80))

    def wait_text(self,term,legacy=False,timeout=60):
        """Wait for literal guest output, optionally accepting the DISPI heuristic."""
        contents=[]
        for guest in self.guests:
            end=time.monotonic()+timeout;accepted=False
            while time.monotonic()<end:
                guest.text('');guest.capture()
                content=self.read_text(guest)
                if term.lower() in content.lower():break
                if legacy and not accepted and 'Continue anyway?' in content:
                    guest.text('y');accepted=True
                    self.report.setdefault('legacyRegisterPrompts',0)
                    self.report['legacyRegisterPrompts']+=1
            else:raise TimeoutError(type(guest).__name__+': missing '+term+'\n'+content)
            contents.append(content)
        return contents

    def check_text(self,name):
        """Check every external-program pixel using the independent IBM oracle."""
        guest=self.guests[0];image=guest.capture().point(lambda c:c>>2)
        state=self.device_state(guest)
        expected=render_text(state)
        if image.size!=expected.size:raise RuntimeError('native text extent differs from IBM oracle')
        different=sum(a!=b for a,b in zip(pixels(image),pixels(expected)))
        record={'case':name,'oracle':'IBM scan-line/plane/DAC model','differentPixels':different,
                'dimensions':image.size,'crtc':state['crtc'],'attribute':state['attr'],
                'pcemuSHA256':hashlib.sha256(image.tobytes()).hexdigest()}
        self.report['results'].append(record)
        image.point(lambda c:(c<<2)|(c>>4)).save(guest.work/(name+'.png'))
        if different:
            ImageChops.difference(image,expected).point(lambda c:min(255,c*4)).save(self.work/(name+'-diff.png'))
            raise RuntimeError(f'{name}: {different} pixels differ from IBM text oracle')
        print(name+': independent full-frame IBM oracle passed',flush=True)

    def blackbook(self):
        """Run unmodified, prebuilt Abrash memory-operation executables."""
        self.shell()
        for name,geometry in [('L24-1',(640,350)),('L25-1',(640,480)),('L25-2',(640,480)),
                              ('L25-3',(640,480)),('L25-4',(640,350)),('L26-1',(640,480)),
                              ('L26-2',(640,480)),('L28-2',(640,350)),('L28-3',(640,480))]:
            self.text(name.lower()+'.exe\n');time.sleep(.5)
            self.compare(name,geometry=geometry)
            self.key('ret');time.sleep(.3)
        for name in ['L30-1','L30-2']:
            self.text(name.lower()+'.exe\n')
            self.compare(name+'-phase1',geometry=(640,350))
            self.key('ret')
            self.compare(name+'-phase2',geometry=(640,350))
            self.key('ret')
            if name=='L30-1':
                # The final phase flips continuously; exit with a third key.
                self.key('ret')
            time.sleep(.3)

    def split(self, graphics=False):
        """Exercise line compare, both panning directions and CRTC display start."""
        self.text('g' if graphics else 't');self.text('s');self.text('12' if graphics else 'b3')
        label='graphics' if graphics else 'text'
        options={'geometry':(640,480)} if graphics else {'columns':80}
        check=(lambda name:self.compare(name,**options)) if graphics else self.check_text
        check(label+'-split')
        for name,action,count in [('hpan','right',3),('vpan','up',3),('split-overflow','pgdn',9),('start','plus',3)]:
            self.key(action,count);check(label+'-'+name)
        if graphics:
            self.text('m');self.compare(label+'-pan-reset',**options)
        self.key('esc',3)

    def xvesa(self):
        """Require zero-error BITS/BURST/MATRIX runs across banks and LFB."""
        self.shell();self.text('x-vesa.com\n')
        self.wait_text('Total video modes')
        self.key('ret');self.key('down',2)
        for model,offset in [('interrupt',0),('far-pointer',16128),('linear',16128)]:
            if model=='far-pointer':self.key('f9')
            if model=='linear':self.key('f10')
            self.key('ret')
            self.wait_text('Starting address:',legacy=True)
            self.text(str(offset)+'\n');self.wait_text('VRAM test size:')
            self.text('256\n');self.wait_text('Pass:')
            self.text('1\n')
            contents=self.wait_text('VRAM Reliability test results',legacy=True)
            for guest,content in zip(self.guests,contents):
                (guest.work/('xvesa-'+model+'.txt')).write_text(content)
                guest.capture().save(guest.work/('xvesa-'+model+'.png'))
                fields={}
                for label in ['VRAM errors','VRAM test offset','VRAM test size','Test pass']:
                    match=re.search(re.escape(label)+r':\s*([0-9,]+)',content)
                    if not match:raise RuntimeError('Missing X-VESA field '+label)
                    fields[label]=int(match[1].replace(',',''))
                if fields!={'VRAM errors':0,'VRAM test offset':offset,'VRAM test size':256,'Test pass':1}:
                    raise RuntimeError('X-VESA memory reliability failure: '+str(fields))
                memory=re.search(r'Memory model:\s*([^\n]+)',content)
                if not memory:raise RuntimeError('Missing X-VESA addressing model')
                fields['memoryModel']=memory[1].strip()
                expected={'interrupt':'Window via Int 10h/Real','far-pointer':'Window via Call pointer/Real','linear':'Linear/Unreal'}[model]
                if fields['memoryModel']!=expected:
                    raise RuntimeError('Wrong X-VESA addressing model: '+fields['memoryModel'])
                self.report['results'].append({'case':'xvesa-'+model+'-'+type(guest).__name__,**fields})
            print('X-VESA '+model+': three patterns, 256 KiB, zero errors on both boards',flush=True)
            self.key('esc')
        self.virtual_screen()

    def virtual_screen(self):
        """Probe a bounded pitch range, then compare the grid and pixel panning."""
        # Reliability leaves the selector on command 3, using the LFB interface.
        self.key('down',4);self.key('ret');self.key('down',3);self.key('ret')
        self.wait_text('Start virtual horizontal scan:')
        for value in ['640','648','400','416']:
            self.text(value+'\n')
        self.key('ret')
        contents=self.wait_text('Horizontal resolution:',legacy=True)
        for guest,content in zip(self.guests,contents):
            (guest.work/'xvesa-virtual-pitches.txt').write_text(content)
            if not re.search(r'Virtual video modes:\s*[1-9]',content):
                raise RuntimeError('X-VESA found no usable virtual pitch')
        self.text('648\n')
        for guest in self.guests:
            end=time.monotonic()+60;accepted=False
            while time.monotonic()<end:
                guest.text('');image=guest.capture()
                # Some firmware programs legacy VGA registers and does not
                # trigger X-VESA's heuristic. Accept it only when presented.
                if not accepted and 'Continue anyway?' in self.read_text(guest):
                    guest.text('y');accepted=True
                    self.report['legacyRegisterPrompts']=self.report.get('legacyRegisterPrompts',0)+1
                if 'usemouseforscrollingtoo' in re.sub('[^a-z]','',ocr(image,guest.work/'virtual-ready.png')):
                    break
            else:raise TimeoutError('Missing X-VESA virtual-screen controls')
        self.compare('xvesa-virtual-grid',geometry=(640,400))
        # QEMU's DISPI scanout truncates its byte start to four-byte units.
        # Native one-pixel origins are checked by the independent ROM suite.
        self.key('right',4);self.key('down',4)
        self.compare('xvesa-virtual-pan',geometry=(640,400))
        # Ensure the comparison exercised a changed viewport, not two ignored keys.
        from PIL import Image
        for guest in self.guests:
            before=Image.open(guest.work/'xvesa-virtual-grid.png')
            after=Image.open(guest.work/'xvesa-virtual-pan.png')
            if before.tobytes()==after.tobytes():raise RuntimeError('X-VESA panning did not move')
        self.key('esc')
        contents=self.wait_text('Display start alignment/bounds test')
        for guest,content in zip(self.guests,contents):
            (guest.work/'xvesa-display-start.txt').write_text(content)
            self.report['results'].append({'case':'xvesa-display-start-'+type(guest).__name__,
                'horizontalAlignment':1,'verticalAlignment':1,'boundsSearch':'UNTESTED (bounded virtual extent)',
                'comparedOrigin':[4,4],'qemuStartByteAlignment':4})
            if not re.search(r'Horizontal alignment:\s*1 Pixel',content) or not re.search(r'Vertical alignment:\s*1 Pixel',content):
                raise RuntimeError('X-VESA display-start readback/alignment failed')
        self.key('esc');self.key('esc')

    def doslib(self):
        """Run DOSLIB's logged mode/memory tests, shortening only timed pauses."""
        self.shell();self.text('rundt.bat\n')
        for guest in self.guests:
            deadline=time.monotonic()+180
            while time.monotonic()<deadline:
                key(guest,'space')
                guest.capture()
                # Log completion, not a transient visual demo, is the pass condition.
                content=self.read_text(guest).lower()
                if 'doslib-done' in content:break
            else:raise TimeoutError(type(guest).__name__+': DOSLIB did not finish')
        self.report['doslibComplete']=True

    def read_doslib_logs(self):
        """Read stopped-guest FAT logs and reject warnings, errors or missing modes."""
        for kind in ['pcemu','qemu']:
            disk=self.work/(kind+'.qcow2')
            with CowFile(disk) as handle:
                fat=FAT(handle)
                entries=list(fat.entries())
                content=None
                for part in ['DIAG','VIDEO','PC','TEST.LOG']:
                    wanted=Writer.entry(part,0,0,0)[:11].decode('ascii')
                    found=next((entry for entry in entries if entry[1]==wanted),None)
                    if found is None:raise RuntimeError(kind+': missing DOSLIB '+part)
                    content=fat.read_file(found[2],found[3] or 0x7fffffff)
                    if part!='TEST.LOG':
                        entries=[]
                        for off in range(0,len(content),32):
                            e=content[off:off+32]
                            if not e or e[0]==0:break
                            if e[0]!=229 and e[11]!=15:
                                entries.append((off,e[:11].decode('ascii'),int.from_bytes(e[26:28],'little'),int.from_bytes(e[28:32],'little')))
            text=content.decode('cp437');(self.work/kind/'doslib.log').write_text(text)
            warnings=[line for line in text.splitlines() if line.startswith(('WARN>','ERROR>'))]
            modes=[mode for mode in [0,1,2,3,4,5,6,7,13,14,15,16,17,18,19]
                   if f'Testing: INT 10h mode {mode} ' in text]
            self.report['results'].append({'case':'doslib-'+kind,'modes':modes,'warnings':warnings,'normalExit':'NORMAL EXIT' in text})
            if warnings or len(modes)!=15 or 'NORMAL EXIT' not in text:
                raise RuntimeError(kind+': DOSLIB warnings/incomplete run: '+str(warnings[:10]))
            print(kind+': DOSLIB 15 modes, no warnings, normal exit',flush=True)

    def close(self, success):
        """Always stop owned processes; retain failure media only for diagnosis."""
        for guest in reversed(self.guests):guest.close()
        if success and self.report.get('doslibComplete'):
            try:self.read_doslib_logs()
            except BaseException as error:
                self.report['status']='failed';self.report['error']=str(error)
                (self.work/'report.json').write_text(json.dumps(self.report,indent=2)+'\n')
                print('Evidence:',self.work,flush=True)
                raise
        (self.work/'report.json').write_text(json.dumps(self.report,indent=2)+'\n')
        if success:
            for path in reversed(self.disks):path.unlink(missing_ok=True)
            self.rom.unlink();self.runner.unlink()
        print('Evidence:',self.work,flush=True)


def run(args):
    test=EdgeRun(args);success=False
    try:
        test.start()
        if args.case in ('all','text'):test.split()
        if args.case in ('all','graphics'):test.split(True)
        if args.case in ('all','blackbook'):test.blackbook()
        if args.case in ('all','doslib'):test.doslib()
        if args.case in ('all','xvesa'):test.xvesa()
        success=True;test.report['status']='passed'
    except BaseException as error:
        test.report['status']='failed';test.report['error']=str(error)
        raise
    finally:test.close(success)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk',type=Path,required=True)
    parser.add_argument('--rom',type=Path,default=ROOT/'build/firmware/pcemu-vga.bin')
    parser.add_argument('--no-jit',action='store_true')
    parser.add_argument('--case',choices=['all','text','graphics','blackbook','doslib','xvesa'],default='all')
    run(parser.parse_args())
