#!/usr/bin/env python3
"""Build the native SwiftUI library frontend for this checkout (Apple Silicon)."""
from pathlib import Path
import plistlib
import platform
import os
import tempfile
import shlex
import subprocess

root = Path(__file__).resolve().parent.parent
subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'), '-G', 'Ninja'], check=True)
subprocess.run(['cmake', '--build', str(root / 'build'), '--target', 'pcemu-native-core'], check=True)
app = root / 'build/PCEmu Library.app/Contents'
(app / 'MacOS').mkdir(parents=True, exist_ok=True)
sources = root / 'platform/macos/PCEmu'
deployment = platform.mac_ver()[0]
flags = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'libzstd'], text=True))
with tempfile.TemporaryDirectory(prefix='swiftui-link-', dir=root / 'build') as temporary:
    executable = Path(temporary) / 'PCEmu'
    subprocess.run(['xcrun', 'swiftc', '-swift-version', '5', '-O', '-parse-as-library',
                    '-target', f'arm64-apple-macosx{deployment}', '-import-objc-header', str(sources / 'NativeBridge.h'),
                    *map(str, sorted(sources.glob('*.swift'))), str(root / 'build/libpcemu-native-core.a'), str(root / 'build/libpcemu-session.a'),
                    '-lc++', '-lz', *flags, '-o', str(executable)], check=True)
    os.replace(executable, app / 'MacOS/PCEmu')
(app / 'Info.plist').write_bytes(plistlib.dumps({
    'CFBundleName': 'PCEmu Library', 'CFBundleDisplayName': 'PCEmu',
    'CFBundleIdentifier': 'local.pcemu.library', 'CFBundleExecutable': 'PCEmu',
    'CFBundlePackageType': 'APPL', 'NSPrincipalClass': 'NSApplication', 'CFBundleShortVersionString': '0.1',
    'LSMinimumSystemVersion': deployment, 'NSHighResolutionCapable': True,
    'PCEmuCheckout': str(root),
}))
subprocess.run(['codesign', '--force', '--sign', '-', str(app.parent)], check=True)
print(app.parent)
