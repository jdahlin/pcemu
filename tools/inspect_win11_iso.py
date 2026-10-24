#!/usr/bin/env python3
"""Check full ARM64 Windows installation media without expanding its WIMs."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET
from inspect_validation_network import pe_architecture

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/media-tools'))
import pycdlib


def wim_info(stream, length):
    header = stream.read(208)
    if len(header) != 208 or header[:8] != b'MSWIM\0\0\0':
        raise ValueError('Expected WIM header')
    size = struct.unpack_from('<Q', header, 72)[0] & ((1 << 56) - 1)
    offset = struct.unpack_from('<Q', header, 80)[0]
    if not size or size > 1024 * 1024 or offset + size > length:
        raise ValueError('Invalid WIM XML resource')
    stream.seek(offset)
    root = ET.fromstring(stream.read(size))
    images = []
    for item in root.findall('IMAGE'):
        if item.findtext('WINDOWS/ARCH') != '12':
            raise ValueError('Non-ARM64 image in installation media')
        images.append({'index': item.get('INDEX'), 'name': item.findtext('NAME'),
                       'architecture': 'ARM64', 'build': item.findtext('WINDOWS/VERSION/BUILD'),
                       'revision': item.findtext('WINDOWS/VERSION/SPBUILD')})
    if not images:
        raise ValueError('WIM contains no images')
    return images


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/windows11-media/inventory.json')
    args = parser.parse_args()
    source = args.iso.resolve(strict=True)
    iso = pycdlib.PyCdlib()
    iso.open(str(source))
    report = {'iso': str(source), 'bytes': source.stat().st_size,
              'elToritoBootCatalog': iso.eltorito_boot_catalog is not None,
              'windowsSetupBootVerified': False, 'windowsInstalled': False, 'files': {}}
    if not report['elToritoBootCatalog']:
        raise ValueError('Installation ISO has no El Torito boot catalog')
    for path in ('/efi/boot/bootaa64.efi', '/sources/boot.wim', '/sources/install.wim'):
        size = iso.get_record(udf_path=path).get_data_length()
        entry = {'bytes': size}
        with iso.open_file_from_iso(udf_path=path) as stream:
            if path.endswith('.wim'):
                entry['images'] = wim_info(stream, size)
            else:
                if size > 8 * 1024 * 1024:
                    raise ValueError('Unexpectedly large EFI loader')
                data = stream.read(size)
                entry.update(architecture=pe_architecture(data), sha256=hashlib.sha256(data).hexdigest())
                if entry['architecture'] != 'ARM64':
                    raise ValueError('EFI loader is not ARM64')
        report['files'][path] = entry
    iso.close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
