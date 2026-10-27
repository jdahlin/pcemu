#!/usr/bin/env python3
"""Build a checksum-pinned, private FreeDOS validation disk without an installer."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import zipfile
from prepare_media import FAT
from fat_stage import Writer

ROOT = Path(__file__).resolve().parents[1]
URL = 'https://download.freedos.org/1.4/FD14-LiteUSB.zip'
SHA256 = '857dcd2ebf9d3d094320154db5fb5b830acba6fb98f981a95a0ca7ab3350338b'
DEFAULT = ROOT/'build/freedos/freedos.qcow2'
FDNET_REVISION = '1fa2d28a107f726bb1cc11087b2b9a68ab345fc8'
FDNET_URL = f'https://gitlab.com/FreeDOS/net/fdnet/-/archive/{FDNET_REVISION}/fdnet-{FDNET_REVISION}.zip'
FDNET_SHA256 = 'b770ce088a23b2b0f223989d74cf8072c4511eb8f5262f5bc7a21eb7a50af6db'


def directory(fat, cluster=None):
    """Read ordinary FAT directory entries, retaining attributes for traversal."""
    if cluster is None:
        fat.f.seek(fat.rootbase);data=fat.f.read(fat.nroot*32)
    else:data=fat.read_file(cluster,0x7fffffff)
    for pos in range(0,len(data),32):
        e=data[pos:pos+32]
        if not e or e[0]==0:break
        if e[0]==229 or e[11]==15:continue
        yield e[:11].decode('ascii'),e[11],int.from_bytes(e[26:28],'little'),int.from_bytes(e[28:32],'little')


def read_path(fat,path):
    """Read an explicit 8.3 guest path; never extract arbitrary archive paths."""
    cluster=None
    parts=path.strip('/').split('/')
    for i,part in enumerate(parts):
        name=Writer.entry(part,0,0,0)[:11].decode('ascii')
        e=next((e for e in directory(fat,cluster) if e[0]==name),None)
        if e is None:raise FileNotFoundError(path)
        if i<len(parts)-1:
            if not e[1]&16:raise ValueError('Not a directory: '+part)
            cluster=e[2]
        else:return fat.read_file(e[2],e[3])


def put_root(fat,name,data):
    """Replace a small existing startup file or allocate a new root file."""
    packed=Writer.entry(name,32,0,0)[:11].decode('ascii')
    if any(e[1]==packed for e in fat.entries()):fat.replace_existing(packed,data)
    else:fat.root_entry(fat.entry(name,32,fat.allocate(data),len(data)));fat.commit()


def startup(fat,config,autoexec):
    """Set both DOS and FreeDOS startup names so per-test scripts take effect."""
    is_free=any(e[1]=='KERNEL  SYS' for e in fat.entries())
    if is_free:
        config+=b'SHELL=C:\\COMMAND.COM C:\\ /E:2048 /P=C:\\AUTOEXEC.BAT\r\n'
        put_root(fat,'FDCONFIG.SYS',config)
        put_root(fat,'FDAUTO.BAT',autoexec)
    put_root(fat,'CONFIG.SYS',config);put_root(fat,'AUTOEXEC.BAT',autoexec)


def prepare(output=DEFAULT):
    """Verify upstream media and build an immutable 32 MiB logical FAT16 parent."""
    folder=ROOT/'build/freedos';folder.mkdir(parents=True,exist_ok=True)
    archive=folder/'FD14-LiteUSB.zip'
    if not archive.exists():
        subprocess.run(['curl','--fail','--location','--retry','2','--max-time','180',URL,'-o',str(archive)],check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=SHA256:raise ValueError('FreeDOS media checksum mismatch')
    network=folder/'fdnet-pinned.zip'
    if not network.exists():
        subprocess.run(['curl','--fail','--location','--retry','2','--max-time','120',FDNET_URL,'-o',str(network)],check=True)
    if hashlib.sha256(network.read_bytes()).hexdigest()!=FDNET_SHA256:raise ValueError('FDNet checksum mismatch')
    output=output.resolve();manifest=output.with_suffix('.json')
    if output.exists():
        if not manifest.exists():raise ValueError('Existing disk has no fixture manifest: '+str(output))
        saved=json.loads(manifest.read_text())
        if saved.get('builderSHA256')==hashlib.sha256(Path(__file__).read_bytes()).hexdigest() and saved.get('diskSHA256')==hashlib.sha256(output.read_bytes()).hexdigest():return output
        raise ValueError('Existing fixture differs; select a new --output or remove the generated fixture')
    raw=folder/'fixture-staging.img'
    if raw.exists():raise ValueError('Staging image exists: '+str(raw))
    try:
        with zipfile.ZipFile(archive) as z:
            raw.write_bytes(z.read('FD14LITE.img'))
            (folder/'upstream-readme.txt').write_bytes(z.read('readme.txt'))
        with raw.open('r+b') as handle:
            fat=Writer(handle)
            # Retain the original media (including source/license packages).
            package=read_path(fat,'PACKAGES/BASE/CTMOUSE.ZIP')
            with zipfile.ZipFile(io.BytesIO(package)) as z:
                names=[n for n in z.namelist() if n.lower().endswith('/ctmouse.exe')]
                if len(names)!=1:raise ValueError('Ambiguous CuteMouse binary')
                drivers={'CTMOUSE.EXE':z.read(names[0])}
            with zipfile.ZipFile(network) as z:
                for name in ['NE2000.COM','DHCP.EXE']:
                    drivers[name]=z.read(f'fdnet-{FDNET_REVISION}/NET/FDNET/'+name)
                (folder/'FDNET-LICENSE').write_bytes(z.read(f'fdnet-{FDNET_REVISION}/LICENSE'))
            fat.put_directory('DRIVERS',drivers)
            startup(fat,b'FILES=40\r\nBUFFERS=20\r\nLASTDRIVE=Z\r\n',
                    b'@ECHO OFF\r\nPATH C:\\FREEDOS\\BIN;C:\\DRIVERS\r\nECHO PCEMU FREEDOS READY\r\n')
            fat.commit()
        output.parent.mkdir(parents=True,exist_ok=True)
        subprocess.run(['qemu-img','convert','-f','raw','-O','qcow2','-c',str(raw),str(output)],check=True)
        record={'distribution':'FreeDOS 1.4 LiteUSB','url':URL,'archiveSHA256':SHA256,
                'verification':'https://download.freedos.org/1.4/verify.txt',
                'fdnet':{'revision':FDNET_REVISION,'url':FDNET_URL,'sha256':FDNET_SHA256},
                'builderSHA256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'diskSHA256':hashlib.sha256(output.read_bytes()).hexdigest(),'logicalBytes':33554432,
                'policy':'Generated bootable fixture; original packages/source/licenses retained; per-run overlays only'}
        manifest.write_text(json.dumps(record,indent=2)+'\n')
    finally:raw.unlink(missing_ok=True)
    return output


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=DEFAULT)
    print(prepare(parser.parse_args().output))
