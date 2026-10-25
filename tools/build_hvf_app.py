#!/usr/bin/env python3
"""Package the built HVF executable as a native macOS SDL application."""
from pathlib import Path
import filecmp
import os
import plistlib
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
app = root / 'build/PCEmu11.app'
contents = app / 'Contents'
macos = contents / 'MacOS'
macos.mkdir(parents=True, exist_ok=True)
info = plistlib.dumps({
    'CFBundleExecutable': 'pcemu-hvf',
    'CFBundleIdentifier': 'local.pcemu11',
    'CFBundleName': 'PCEmu11',
    'CFBundlePackageType': 'APPL',
    'CFBundleVersion': '1',
    'NSHighResolutionCapable': True,
})
binary = macos / 'pcemu-hvf'
plist = contents / 'Info.plist'
source = root / 'build/pcemu-hvf'
if not (binary.exists() and plist.exists() and plist.read_bytes() == info
        and filecmp.cmp(source, binary, shallow=False)):
    # Replace the executable's inode rather than overwriting a running VM.
    fd, temporary = tempfile.mkstemp(prefix='.pcemu-hvf-', dir=macos)
    os.close(fd)
    try:
        shutil.copy2(source, temporary)
        os.replace(temporary, binary)
    finally:
        Path(temporary).unlink(missing_ok=True)
    plist.write_bytes(info)
    subprocess.run(['codesign', '--force', '--sign', '-', '--entitlements',
                    str(root / 'platform/macos/hypervisor.entitlements'),
                    str(app)], check=True)
print(app)
