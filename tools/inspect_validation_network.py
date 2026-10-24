#!/usr/bin/env python3
"""Inventory network components in Validation OS WIMs without changing the media.

Reads all staged INF version classes, then extracts only selected networking
binaries in memory to check PE architecture. This is an offline inventory, not a
running-guest network test and not an inventory of a subsequently modified VHDX.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = {
    'stack': ('ndis.sys', 'netio.sys', 'tcpip.sys', 'tcpipreg.sys', 'afd.sys',
              'nsiproxy.sys', 'fwpkclnt.sys', 'msquic.sys'),
    'frameworks': ('netadaptercx.sys', 'netcxrd.sys', 'ndisuio.sys'),
    'debugAdapters': ('kdnic.sys', 'kdnic_legacy.sys'),
    'adapterCandidates': ('netkvm.sys', 'netvsc.sys', 'rndismp.sys', 'usb8023.sys',
                          'usb8023x.sys', 'usbncm.sys', 'rt640x64.sys', 'e1g6032e.sys'),
}
TOOLS = ('ipconfig.exe', 'ping.exe', 'netsh.exe', 'pnputil.exe', 'curl.exe',
         'powershell.exe', 'dism.exe')


def decode_inf(data):
    return data.decode('utf-16') if data.startswith((b'\xff\xfe', b'\xfe\xff')) else data.decode('utf-8-sig')


def inf_summary(text):
    section = ''
    info = {'class': None, 'provider': None, 'hardwareIds': [], 'services': [],
            'serviceBinaries': [], 'descriptions': []}
    for raw in text.splitlines():
        line = raw.split(';', 1)[0].strip()
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1].lower()
        elif '=' in line:
            key, value = (s.strip() for s in line.split('=', 1))
            key = key.lower()
            if section == 'version' and key in ('class', 'provider'):
                info[key] = value.strip('"')
            if key == 'addservice':
                info['services'].append(value.split(',')[0].strip())
            if key == 'servicebinary':
                info['serviceBinaries'].append(value.strip('"'))
            if section.startswith('strings') and ('devicedesc' in key or 'friendlyname' in key):
                info['descriptions'].append(value.strip('"'))
            for match in re.finditer(r'(?i)\b(?:PCI|USB|ROOT|VMBUS|SWD|KDNIC)\\[^,\s;"\\]+(?:\\[^,\s;"\\]+)*', value):
                info['hardwareIds'].append(match.group())
    for key in ('hardwareIds', 'services', 'serviceBinaries', 'descriptions'):
        info[key] = sorted(set(info[key]))
    return info


def pe_architecture(data):
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Expected PE image')
    offset = struct.unpack_from('<I', data, 60)[0]
    if offset + 6 > len(data) or data[offset:offset + 4] != b'PE\0\0':
        raise ValueError('Invalid PE header')
    machine = struct.unpack_from('<H', data, offset + 4)[0]
    return {0xaa64: 'ARM64', 0x8664: 'AMD64', 0x14c: 'I386'}.get(machine, f'0x{machine:04x}')


def inspect(wimlib, wim, image):
    def run(*args):
        return subprocess.run([wimlib, *args], check=True, capture_output=True).stdout

    def extract(path):
        return run('extract', str(wim), str(image), path, '--to-stdout')

    paths = [p for p in run('dir', str(wim), str(image)).decode().splitlines() if p.startswith('/')]
    lookup = {p.lower(): p for p in paths}
    packages = []
    for path in paths:
        if not path.lower().startswith('/windows/inf/') or not path.lower().endswith('.inf'):
            continue
        info = inf_summary(decode_inf(extract(path)))
        if (info['class'] or '').lower() in ('net', 'nettrans', 'netservice', 'netclient') or path.lower().endswith('/netcxrd.inf'):
            packages.append({'path': path, **info})
    groups = {}
    for group, names in COMPONENTS.items():
        groups[group] = []
        for name in names:
            path = lookup.get('/windows/system32/drivers/' + name)
            entry = {'name': name, 'present': bool(path)}
            if path:
                data = extract(path)
                entry.update(path=path, bytes=len(data), architecture=pe_architecture(data),
                             sha256=hashlib.sha256(data).hexdigest())
            groups[group].append(entry)
    return {'image': image, 'networkInfPackages': packages, 'components': groups,
            'tools': {name: lookup.get('/windows/system32/' + name) for name in TOOLS},
            'systemDriverNames': sorted(p.rsplit('/', 1)[-1] for p in paths
                if re.fullmatch(r'/windows/system32/drivers/[^/]+\.sys', p, re.I)),
            'hardwareNetworkDriverInfPackages': [p['path'] for p in packages
                if any(h.lower().startswith(('pci\\', 'usb\\', 'vmbus\\')) for h in p['hardwareIds'])]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wim', type=Path, default=ROOT / 'media/validation-os-arm64/ValidationOS.wim')
    parser.add_argument('--wimlib', default=shutil.which('wimlib-imagex') or
                        str(ROOT / 'build/media-tools/wimlib/wimlib-imagex'))
    parser.add_argument('--output', type=Path, default=ROOT / 'build/validation-network/inventory.json')
    args = parser.parse_args()
    report = {'wim': str(args.wim.resolve(strict=True)), 'method': 'read-only offline WIM inventory',
              'guestNetworkVerified': False, 'vhdxInspected': False,
              'images': [inspect(args.wimlib, args.wim, image) for image in (1, 2)]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    for image in report['images']:
        print(f"Image {image['image']}: " + ', '.join(p['path'].rsplit('/', 1)[-1] for p in image['networkInfPackages']))
        print('PCI/USB/VMBus NIC INF packages:', image['hardwareNetworkDriverInfPackages'])
        for group, components in image['components'].items():
            print(f"  {group}: " + ', '.join(p['name'] for p in components if p['present']))
        print('  tools:', ', '.join(name for name, path in image['tools'].items() if path))
    print(args.output)


if __name__ == '__main__':
    main()
