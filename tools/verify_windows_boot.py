#!/usr/bin/env python3
"""Verify the installed Windows guest report, including active VirtIO I/O.

A complete report must come from the installed-system FIRSTBOOT.CMD. WinPE
probe logs, driver staging, and process exit status alone are insufficient.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def verify(report, build='26300', user='PCEmu'):
    errors = []
    def require(condition, explanation):
        if not condition:
            errors.append(explanation)
    require('PCEMU_INSTALLED_WINDOWS_BOOT' in report, 'Missing installed-system start marker')
    require('PCEMU_INSTALLED_WINDOWS_REPORT_END' in report, 'Missing complete-report end marker')
    require(re.search(r'Microsoft Windows \[Version 10\.0\.' + re.escape(build) + r'\.', report), 'Unexpected Windows version')
    require(re.search(r'CurrentBuildNumber\s+REG_SZ\s+' + re.escape(build) + r'\b', report), 'Missing installed Windows build registry evidence')
    require(re.search(r'^SYSTEMDRIVE=C:\s*$', report, re.M | re.I), 'System drive is not C:')
    for field in ('device', 'osdevice'):
        require(re.search(r'^' + field + r'\s+partition=C:\s*$', report, re.M | re.I), f'BCD {field} does not identify the installed partition')
    require(re.search(r'^path\s+\\Windows\\system32\\winload\.efi\s*$', report, re.M | re.I), 'Missing installed EFI OS loader')
    require(re.search(r'^PCEMU_FIRST_LOGON=' + re.escape(user) + r'\s*$', report, re.M | re.I), 'Local test account has not logged on')
    devices = {}
    blocks = re.split(r'\r?\n\s*\r?\n', report)
    for identifier, name in (('1042', 'storage'), ('1041', 'network'), ('1044', 'rng')):
        matches = [block for block in blocks if re.search(r'Instance ID:\s+PCI\\VEN_1AF4&DEV_' + identifier + r'&', block, re.I)]
        require(len(matches) == 1, f'Expected one VirtIO {name} device')
        if len(matches) == 1:
            block = matches[0]
            started = re.search(r'^Status:\s+Started\s*$', block, re.M) is not None
            require(started, f'VirtIO {name} device did not start')
            require('Problem Code:' not in block, f'VirtIO {name} reports a device problem')
            devices[name] = {'pciDevice': identifier, 'started': started}
    require(re.search(r'IPv4 Address[^\r\n]*10\.0\.2\.15\b', report), 'Missing user-network IPv4 lease')
    require(re.search(r'^HTTP_GET_RC=0\s*$', report, re.M), 'HTTP probe did not succeed')
    require('PCEMU_WINDOWS_HTTP_OK' in report, 'Missing HTTP response payload')
    require('PCEMU_VIRTIO_RNG_PROVIDER_OK bytes=128' in report, 'Explicit VirtIO CNG provider I/O is unverified')
    require(re.search(r'^RNG_CHECK_RC=0\s*$', report, re.M), 'RNG checker did not succeed')
    require('PCEMU_VIRTIO_RNG_PROVIDER_FAILED' not in report, 'RNG provider failure is present')
    return {'verified': not errors, 'expectedBuild': build, 'expectedUser': user, 'devices': devices, 'errors': errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--build', default='26300')
    parser.add_argument('--user', default='PCEmu')
    args = parser.parse_args()
    if args.report.stat().st_size > 8 * 1024 * 1024:
        parser.error('Expected a bounded diagnostic report')
    raw = args.report.read_bytes()
    report = raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8')
    result = verify(report, args.build, args.user)
    result.update(report=str(args.report.resolve()), sha256=hashlib.sha256(raw).hexdigest())
    print(json.dumps(result, indent=2))
    return 0 if result['verified'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
