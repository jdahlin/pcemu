#!/usr/bin/env python3
"""Fetch checksum-pinned external VGA diagnostics into ignored build storage."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import zipfile
import struct
import tarfile
from prepare_media import FAT

ROOT = Path(__file__).resolve().parents[1]
VGATEST_URL = 'https://github.com/barotto/VGATEST/releases/download/v1.5/VGATEST-1.5-DOS.zip'
VGATEST_SHA = 'fd5a229310a05e18008864642b510e6033999df17111f4ce66fa408a50ac9681'
XVESA_REVISION = '96ff78f92f7b6832046f42486641bf9dafbc29b6'
XVESA_URL = f'https://raw.githubusercontent.com/Marco-Pistella/X-VESA/{XVESA_REVISION}/Release/X-VESA.COM'
XVESA_SHA = '15040fafa9749139e97ca51f719ebffc12d33a6e2fa4d2739854573320bac366'


def fetch(url, digest, destination):
    """Download with TLS verification and reject altered content before use."""
    if destination.exists() and hashlib.sha256(destination.read_bytes()).hexdigest() == digest:
        return destination.read_bytes()
    data = subprocess.check_output(['curl', '--fail', '--location', '--silent', '--show-error',
                                    '--max-time', '60', url])
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f'Diagnostic checksum mismatch: {url}')
    destination.write_bytes(data)
    return data


def prepare(folder):
    """Extract explicit ZIP members only; preserve licenses beside the binaries."""
    folder.mkdir(parents=True, exist_ok=True)
    data = fetch(VGATEST_URL, VGATEST_SHA, folder / 'vgatest-1.5.zip')
    files = {}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for name in ('VGATEST0.EXE', 'VGATEST.EXE', 'dos4gw.exe', 'COPYING', 'README', 'VERSION'):
            content = archive.read(name)
            (folder / name.upper()).write_bytes(content)
            files[name.upper()] = hashlib.sha256(content).hexdigest()
    data = fetch(XVESA_URL, XVESA_SHA, folder / 'X-VESA.COM')
    files['X-VESA.COM'] = hashlib.sha256(data).hexdigest()
    # The release is GPL-3.0; retain the unmodified source license in the cache.
    license_url = f'https://raw.githubusercontent.com/Marco-Pistella/X-VESA/{XVESA_REVISION}/LICENSE'
    fetch(license_url, '3bf2469479598343386b0388eba697581f7b06e54c1d9e727b833eb242d32cab',
          folder / 'XVESA-LICENSE')
    manifest = {'vgatest': {'version': '1.5', 'url': VGATEST_URL, 'sha256': VGATEST_SHA,
                           'license': 'GPL-3.0-or-later'},
                'xvesa': {'revision': XVESA_REVISION, 'url': XVESA_URL, 'sha256': XVESA_SHA,
                          'license': 'GPL-3.0'}, 'files': files}
    (folder / 'diagnostics.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


DOSLIB_REVISION = '473980b3f58d2dc4a6e614b843aba1cec4c03df1'
DOSLIB_ARCHIVE = f'https://github.com/joncampbell123/doslib/releases/download/doslib-20250729/doslib-20250729-110756-commit-{DOSLIB_REVISION}-binary.tar.xz'
DOSLIB_SHA = '9b58757c3de68fd72c9ce13aeb4694c8f6e07e866548acb7178e276cf1704759'
DOSLIB_FILES = {
    'test-fw/video/pc/dos86l/test.exe': ('DTEST.EXE','9c1e1c076c2df47676b7a42af849df3b98fcf6cfc1a486c6a2350edcf0a8ae18'),
    'test-fw/video/pc/dos86l/vgaacdac.exe': ('VGAACDAC.EXE','30ad8bb431627c4a4e6d0de42cd625b0a781f1dd8b5a32f83ea7907b0d915592'),
    'test-fw/video/pc/dos86l/vga256t1.exe': ('VGA256T1.EXE','cdc70a8e9ae847d8dd3571179fec996374a8b4ab0eafb76c1600a2b54f77ad41'),
    'test-fw/video/pc/dos86l/vga256b4.exe': ('VGA256B4.EXE','a5c3420fd9d7ccb02f60046ee559b6888ba428f33d5e574433604f96d41fee38'),
    'hw/vga/dos86l/tmodeset.exe': ('TMODESET.EXE','5352bcfb4ec8b8a8bd0ddf1757ecb12d4f984d92ab0b7366b893112c138e9c0e'),
}
BLACKBOOK_REVISION = 'cd3433748b09837a4c60ca7066f3642772b2d711'
BLACKBOOK_URL = f'https://raw.githubusercontent.com/jeffpar/pcjs-diskettes/{BLACKBOOK_REVISION}/pcx86/test/vga/blackbook/VGABIN.json'
BLACKBOOK_SHA = 'db3ab25f00789f358005ea7b34f9cd8f6ced5833814f7b412b8af1d9db85c8da'


def prepare_edges(folder):
    """Fetch unmodified edge diagnostics; retain only selected small DOS files.

    DOSLIB's optional release archive is large because it covers many devices,
    architectures and memory models. Extract explicit members and remove the
    archive after validating individual hashes; later runs use that small cache.
    Abrash binaries stay in ignored storage under their upstream source terms.
    """
    folder.mkdir(parents=True,exist_ok=True)
    disk_json=fetch(BLACKBOOK_URL,BLACKBOOK_SHA,folder/'VGABIN.json')
    disk=json.loads(disk_json);raw=bytearray()
    for cylinder in disk['diskData']:
        for head in cylinder:
            for sector in head:
                data=sector['d'];data=data+[data[-1]]*(sector['l']//4-len(data))
                raw.extend(b''.join(struct.pack('<I',value&0xffffffff) for value in data))
    fat=FAT(io.BytesIO(raw));files={}
    for _,name,cluster,size in fat.entries():
        if name.endswith('EXE'):
            filename=name[:8].strip()+'.EXE';content=fat.read_file(cluster,size)
            (folder/filename).write_bytes(content)
            files[filename]=hashlib.sha256(content).hexdigest()
    if any(not (folder/name).exists() or hashlib.sha256((folder/name).read_bytes()).hexdigest()!=sha
           for name,sha in DOSLIB_FILES.values()):
        archive=folder/'doslib.tar.xz'
        fetch(DOSLIB_ARCHIVE,DOSLIB_SHA,archive)
        with tarfile.open(archive) as source:
            for member,(name,sha) in DOSLIB_FILES.items():
                content=source.extractfile('doslib/'+member).read()
                if hashlib.sha256(content).hexdigest()!=sha:raise ValueError('DOSLIB member mismatch: '+name)
                (folder/name).write_bytes(content)
        archive.unlink()
    # Remove the research cache too when all selected files already validate.
    (folder/'doslib.tar.xz').unlink(missing_ok=True)
    fetch(f'https://raw.githubusercontent.com/joncampbell123/doslib/{DOSLIB_REVISION}/LICENSE',
          '9b872a8a070b8ad329c4bd380fb1bf0000f564c75023ec8e1e6803f15364b9e9',folder/'DOSLIB-LICENSE')
    manifest={'doslib':{'revision':DOSLIB_REVISION,'url':DOSLIB_ARCHIVE,'archiveSHA256':DOSLIB_SHA,
                         'license':'LGPL-2.1 (see cached upstream LICENSE)'},
              'blackbook':{'revision':BLACKBOOK_REVISION,'url':BLACKBOOK_URL,'sha256':BLACKBOOK_SHA,
                            'source':'https://www.pcjs.org/software/pcx86/test/vga/blackbook/',
                            'terms':'Upstream Abrash book/source terms; no binaries bundled with PCEmu'},
              'files':dict(files,**{name:sha for name,sha in DOSLIB_FILES.values()})}
    (folder/'edge-diagnostics.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edges',action='store_true',help='also fetch pinned DOSLIB/Abrash cases')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/vga-diagnostics/tools')
    args = parser.parse_args()
    prepare(args.output)
    if args.edges:prepare_edges(args.output)
    print(args.output.resolve())
