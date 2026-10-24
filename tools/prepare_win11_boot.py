#!/usr/bin/env python3
"""Stage Windows Setup's unchanged boot WIM on a private sparse FAT16 NVMe disk.

Only boot files are copied; install.wim stays in the original ISO. This boots
Windows PE/Setup for bring-up, not a complete Windows installation source.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from fat_stage import Writer
from inspect_validation_network import pe_architecture
from inspect_win11_iso import wim_info

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/media-tools'))
import pycdlib


def format_disk(path):
    sectors, start, spc, spf = 2 * 1024 * 1024, 2048, 64, 128
    volume = sectors - start
    mbr = bytearray(512)
    mbr[446] = 0x80; mbr[450] = 0xef
    struct.pack_into('<II', mbr, 454, start, volume); mbr[510:] = b'\x55\xaa'
    boot = bytearray(512); boot[:11] = b'\xeb\x3c\x90MSDOS5.0'
    struct.pack_into('<HBHBHHBH', boot, 11, 512, spc, 1, 2, 512, 0, 0xf8, spf)
    struct.pack_into('<HHII', boot, 24, 63, 255, start, volume)
    boot[36:39] = b'\x80\x00\x29'; struct.pack_into('<I', boot, 39, 0x57494e50)
    boot[43:54] = b'PCEMU WINPE'; boot[54:62] = b'FAT16   '; boot[510:] = b'\x55\xaa'
    with path.open('xb') as f:
        f.truncate(sectors * 512); f.write(mbr); f.seek(start * 512); f.write(boot)
        for fat in range(2):
            f.seek((start + 1 + fat * spf) * 512); f.write(b'\xf8\xff\xff\xff')


def allocate_stream(writer, stream, size):
    count = (size + writer.cluster_bytes - 1) // writer.cluster_bytes
    first = writer.next_free
    if not count or first + count > min(writer.cluster_limit, len(writer.fat) // 2, 0xfff0):
        raise ValueError('Boot disk full or empty payload')
    for i in range(count):
        if struct.unpack_from('<H', writer.fat, (first + i) * 2)[0]:
            raise ValueError('Expected fresh contiguous allocation')
        struct.pack_into('<H', writer.fat, (first + i) * 2, first + i + 1 if i + 1 < count else 0xffff)
    writer.next_free += count
    writer.f.seek(writer.database + (first - 2) * writer.cluster_bytes)
    digest = hashlib.sha256(); left = size
    while left:
        data = stream.read(min(left, 4 * 1024 * 1024))
        if not data:
            raise ValueError('Truncated ISO boot payload')
        writer.f.write(data); digest.update(data); left -= len(data)
    return first, digest.hexdigest()


def directory_entry(name, cluster):
    if name != 'MICROSOFT':
        return Writer.entry(name, 16, cluster, 0)
    short = Writer.entry('MICROS~1', 16, cluster, 0)
    checksum = 0
    for byte in short[:11]:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 255
    long = bytearray(32)
    long[0] = 0x41; long[11] = 15; long[13] = checksum
    text = (name + '\0').encode('utf-16le').ljust(26, b'\xff')
    long[1:11] = text[:10]; long[14:26] = text[10:22]; long[28:32] = text[22:26]
    return bytes(long) + short


def prepare(iso_path, output):
    iso = pycdlib.PyCdlib(); iso.open(str(iso_path))
    try:
        return prepare_open_iso(iso, iso_path, output)
    finally:
        iso.close()


def prepare_open_iso(iso, iso_path, output):
    # Inspect architecture before creating a derived disk.
    with iso.open_file_from_iso(udf_path='/efi/boot/bootaa64.efi') as f:
        data = f.read(4096)
        if pe_architecture(data) != 'ARM64':
            raise ValueError('Windows boot manager is not ARM64')
        pe = struct.unpack_from('<I', data, 60)[0]
        if pe + 94 > len(data) or struct.unpack_from('<H', data, pe + 92)[0] != 10:
            raise ValueError('Boot loader is not an EFI application')
    size = iso.get_record(udf_path='/sources/boot.wim').get_data_length()
    with iso.open_file_from_iso(udf_path='/sources/boot.wim') as f:
        images = wim_info(f, size)
    format_disk(output)
    writer = Writer(output)
    records, directories = [], {}
    def directory(path):
        if path not in directories:
            cluster = writer.allocate(bytes(writer.cluster_bytes))
            directories[path] = {'cluster': cluster, 'entries': []}
            parent, _, name = path.rpartition('/')
            entry = directory_entry(name, cluster)
            if parent:
                directory(parent)['entries'].append(entry)
            else:
                writer.root_entry(entry)
        return directories[path]
    try:
        for source, target in (
            ('/efi/boot/bootaa64.efi', 'EFI/BOOT/BOOTAA64.EFI'),
            ('/efi/microsoft/boot/bcd', 'EFI/MICROSOFT/BOOT/BCD'),
            ('/boot/boot.sdi', 'BOOT/BOOT.SDI'),
            ('/sources/boot.wim', 'SOURCES/BOOT.WIM'),
        ):
            parent, _, name = target.rpartition('/')
            d = directory(parent)
            size = iso.get_record(udf_path=source).get_data_length()
            with iso.open_file_from_iso(udf_path=source) as stream:
                cluster, digest = allocate_stream(writer, stream, size)
            d['entries'].append(writer.entry(name, 32, cluster, size))
            records.append({'source': source, 'target': target, 'bytes': size, 'sha256': digest})
        for path, d in directories.items():
            parent = path.rpartition('/')[0]
            data = (writer.entry('.          ', 16, d['cluster'], 0) +
                    writer.entry('..         ', 16, directories[parent]['cluster'] if parent else 0, 0) +
                    b''.join(d['entries']))
            if len(data) > writer.cluster_bytes:
                raise ValueError('Boot directory exceeds one cluster')
            writer.f.seek(writer.database + (d['cluster'] - 2) * writer.cluster_bytes)
            writer.f.write(data.ljust(writer.cluster_bytes, b'\0'))
        writer.commit()
    finally:
        writer.f.close()
    return {'iso': str(iso_path), 'bootDisk': str(output), 'wimImages': images, 'files': records,
            'installImageIncluded': False, 'windowsSetupBootVerified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=ROOT / 'media/windows11-arm64/Windows11_Client_arm64_en-us_26300_9457.iso')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/windows11-media/setup-efi-boot.raw')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists() or args.output.is_symlink():
        parser.error('Refusing to overwrite an existing boot disk')
    partial = args.output.with_suffix(args.output.suffix + '.part')
    report = prepare(args.iso.resolve(strict=True), partial)
    partial.rename(args.output)
    report['bootDisk'] = str(args.output)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
