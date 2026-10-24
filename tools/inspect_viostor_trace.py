#!/usr/bin/env python3
"""Read WPP message formats from a matching upstream PDB and inspect a guest ETL.

Requires LLVM llvm-pdbutil and optional dissect.etl in build/media-tools.
No Microsoft or upstream driver code is copied into this repository.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/media-tools'))


def formats(symbols):
    result = {}
    for annotation in symbols.split('S_ANNOTATION')[1:]:
        match = re.search(r'TMF:\s*([0-9a-fA-F-]{36})[^\n]*\n\s*#typev \S+ (\d+) "([^\n]*)" //', annotation)
        if match:
            result[(match[1].lower(), int(match[2]))] = match[3]
    return result


def message_data(header):
    # dissect.etl preserves the WPP optional header in payload. Skip it according
    # to TRACE_MESSAGE flags rather than assuming a fixed provider/header size.
    flags = header.event_property
    if flags & ~0xeb:
        raise ValueError(f'Unsupported WPP message properties: {flags:#x}')
    offset = (4 if flags & 1 else 0) + (16 if flags & 2 else 0)
    offset += (8 if flags & 8 else 0) + (8 if flags & 0x20 else 0)
    payload = bytes(header.payload)
    if offset > len(payload):
        raise ValueError('Truncated WPP message metadata')
    return payload[offset:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--etl', type=Path, required=True)
    parser.add_argument('--pdb', type=Path, required=True)
    args = parser.parse_args()
    if args.etl.stat().st_size > 8 * 1024 * 1024:
        parser.error('Expected a bounded guest diagnostic ETL')
    from dissect.etl import ETL
    output = subprocess.run(['llvm-pdbutil', 'dump', '--symbols', str(args.pdb)],
                            check=True, capture_output=True, text=True, timeout=30)
    descriptions = formats(output.stdout)
    with args.etl.open('rb') as stream:
        for record in ETL(stream):
            header = record.header
            if type(header).__name__ != 'MessageTraceHeader':
                continue
            guid, identifier = str(header.provider_id).lower(), header.id
            data = message_data(header)
            print(json.dumps({'guid': guid, 'message': identifier,
                              'format': descriptions.get((guid, identifier)),
                              'argumentsHex': data.hex()}))


if __name__ == '__main__':
    main()
