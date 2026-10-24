#!/usr/bin/env python3
"""Build the original ARM64 WinPE helper using LLVM and generated import libraries."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DLLS = {
    'kernel32': 'GetStdHandle WriteFile ReadFile CreateFileW GetFileSizeEx CloseHandle GetLastError GetCommandLineW ExitProcess',
    'winhttp': 'WinHttpOpen WinHttpSetTimeouts WinHttpConnect WinHttpOpenRequest WinHttpSendRequest WinHttpWriteData WinHttpReceiveResponse WinHttpReadData WinHttpQueryHeaders WinHttpCloseHandle',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/windows-guest')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    clang = shutil.which('clang')
    dlltool = shutil.which('llvm-dlltool')
    linker = shutil.which('lld-link') or '/opt/homebrew/opt/lld/bin/lld-link'
    if not clang or not dlltool or not Path(linker).is_file():
        parser.error('LLVM clang/llvm-dlltool and lld-link are required')
    libraries = []
    for dll, names in DLLS.items():
        definition = args.output / (dll + '.def')
        definition.write_text(f'LIBRARY {dll}.dll\nEXPORTS\n' + '\n'.join(names.split()) + '\n')
        library = args.output / (dll + '.lib')
        subprocess.run([dlltool, '-m', 'arm64', '-d', str(definition), '-l', str(library)], check=True)
        libraries.append(str(library))
    obj = args.output / 'guest-http.obj'
    exe = args.output / 'guest-http.exe'
    subprocess.run([clang, '--target=aarch64-pc-windows-msvc', '-ffreestanding', '-fno-stack-protector',
                    '-Wall', '-Wextra', '-Werror', '-O2', '-c', str(ROOT / 'tools/windows/guest_http.c'), '-o', str(obj)], check=True)
    subprocess.run([linker, '/machine:arm64', '/subsystem:console', '/entry:mainCRTStartup',
                    '/nodefaultlib', '/out:' + str(exe), str(obj), *libraries], check=True)
    print(exe)


if __name__ == '__main__':
    main()
