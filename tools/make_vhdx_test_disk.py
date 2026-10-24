#!/usr/bin/env python3
"""Wrap a small RAW fixture in an original dynamic VHDX for integration tests.

Test-media builder, not an import/conversion interface for user disks.
"""
from pathlib import Path
import struct
import uuid

MB = 1024 * 1024


def crc32c(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82f63b78 if value & 1 else 0)
    return value ^ 0xffffffff


def seal(data):
    struct.pack_into('<I', data, 4, 0)
    struct.pack_into('<I', data, 4, crc32c(data))


def create(source, target):
    source, target = Path(source), Path(target)
    size = source.stat().st_size
    if not size or size % 512 or size > 128 * MB:
        raise ValueError('Fixture must be sector aligned and at most 128 MiB')
    blocks = (size + MB - 1) // MB
    with source.open('rb') as raw, target.open('xb') as output:
        output.truncate(4 * MB)
        output.write(b'vhdxfile')
        file_guid, data_guid = uuid.uuid4().bytes_le, uuid.uuid4().bytes_le
        for sequence, offset in ((1, 65536), (2, 131072)):
            header = bytearray(4096)
            header[:4] = b'head'
            struct.pack_into('<Q16s16s', header, 8, sequence, file_guid, data_guid)
            struct.pack_into('<HHIQ', header, 64, 0, 1, MB, MB)
            seal(header)
            output.seek(offset)
            output.write(header)
        regions = bytearray(65536)
        regions[:4] = b'regi'
        struct.pack_into('<I', regions, 8, 2)
        for n, (guid, at) in enumerate((('2dc27766-f623-4200-9d64-115e9bfd4a08', 3 * MB),
                                       ('8b7ca206-4790-4b9a-b8fe-575f050f886e', 2 * MB))):
            struct.pack_into('<16sQII', regions, 16 + n * 32, uuid.UUID(guid).bytes_le, at, MB, 1)
        seal(regions)
        for offset in (192 * 1024, 256 * 1024):
            output.seek(offset)
            output.write(regions)
        metadata = bytearray(65576)
        metadata[:8] = b'metadata'
        struct.pack_into('<H', metadata, 10, 5)
        items = [('caa16737-fa36-4d43-b3b6-33f0aa44e76b', struct.pack('<II', MB, 0), 4),
                 ('2fa54224-cd1b-4876-b211-5dbed83bf4b8', struct.pack('<Q', size), 6),
                 ('8141bf1d-a96f-4709-ba47-f233a8faab5f', struct.pack('<I', 512), 6),
                 ('cda348c7-445d-4471-9cc9-e9885251c556', struct.pack('<I', 4096), 6),
                 ('beca12ab-b2e6-4523-93ef-c309e000c746', uuid.uuid4().bytes_le, 6)]
        at = 65536
        for n, (guid, value, flags) in enumerate(items):
            struct.pack_into('<16sIIII', metadata, 32 + n * 32, uuid.UUID(guid).bytes_le,
                             at, len(value), flags, 0)
            metadata[at:at + len(value)] = value
            at += len(value)
        output.seek(2 * MB)
        output.write(metadata)
        bat = bytearray(blocks * 8)
        end = 4 * MB
        for block in range(blocks):
            contents = raw.read(MB)
            if not any(contents):
                continue
            struct.pack_into('<Q', bat, block * 8, end | 6)
            # Keep zero portions sparse rather than expanding a 64 MiB fixture.
            for offset in range(0, len(contents), 4096):
                chunk = contents[offset:offset + 4096]
                if any(chunk):
                    output.seek(end + offset)
                    output.write(chunk)
            end += MB
        output.truncate(end)
        output.seek(3 * MB)
        output.write(bat)
    return target
