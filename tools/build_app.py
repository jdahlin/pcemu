#!/usr/bin/env python3
"""Create a local double-click launcher for this checkout."""
from pathlib import Path
import argparse
import plistlib
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--windows95', action='store_true')
a = p.parse_args()
name = 'PCEmu95' if a.windows95 else 'PCEmu'
script = 'run95.sh' if a.windows95 else 'run.sh'
root = Path(__file__).resolve().parent.parent
subprocess.run(['make', '-s'], cwd=root, check=True)
app = root / f'build/{name}.app/Contents'
(app / 'MacOS').mkdir(parents=True, exist_ok=True)
(app / 'Info.plist').write_bytes(plistlib.dumps({
    'CFBundleName': name, 'CFBundleDisplayName': name,
    'CFBundleIdentifier': 'local.' + name.lower(), 'CFBundleExecutable': name,
    'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '0.1',
    'NSHighResolutionCapable': True,
}))
launcher = app / f'MacOS/{name}'
launcher.write_text('#!/bin/bash\nset -e\ncd "$(dirname "$0")/../../../.."\nexec ./' + script + '\n')
launcher.chmod(0o755)
print(root / f'build/{name}.app')
