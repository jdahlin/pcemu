#!/usr/bin/env python3
"""Attribute macOS stack samples using an experimental emitted-code region map.

Maps must come from the exact sampled process. Reused/overlapping addresses are
rejected: aggregate sample trees cannot distinguish their code lifetimes.
"""
import argparse
import bisect
import collections
import hashlib
import json
import re
from pathlib import Path

from analyze_samples import parse


def validate_process(sample_text, blocks):
    process = re.search(r'^Process:\s+.*\[(\d+)\]', sample_text, re.MULTILINE)
    if not process or {b.get('pid') for b in blocks} != {int(process[1])}:
        raise ValueError('Sample and code map must identify the same process PID')


class CodeMap:
    def __init__(self, blocks):
        self.blocks = sorted(blocks, key=lambda b: b['start'])
        self.starts = [b['start'] for b in self.blocks]
        previous_end = -1
        for b in self.blocks:
            if b['start'] < previous_end:
                raise ValueError('Reused/overlapping code addresses; attribution is ambiguous')
            if b['end'] <= b['start']:
                raise ValueError('Empty code range')
            offsets = [r['offset'] for r in b['regions']]
            if not offsets or offsets[0] != 0 or offsets != sorted(offsets):
                raise ValueError('Invalid region offsets')
            if offsets[-1] >= b['end'] - b['start']:
                raise ValueError('Region outside code range')
            b['_offsets'] = offsets
            previous_end = b['end']

    def lookup(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i < 0 or address >= self.blocks[i]['end']:
            return None
        block = self.blocks[i]
        j = bisect.bisect_right(block['_offsets'], address - block['start']) - 1
        return block['regions'][j]


def label(region):
    kind = region['kind']
    if kind != 'Memory':
        return kind
    if region.get('memoryPush'):
        return 'Memory/PUSH source and stack'
    if region.get('call'):
        return 'Memory/CALL'
    if region.get('return'):
        return 'Memory/RET'
    if region.get('stackDelta'):
        return 'Memory/stack'
    return 'Memory/' + ('write' if region.get('store') else 'read')


def attribute(nodes, code_map):
    groups = collections.Counter()
    main_samples = 0
    mapped = 0
    for node in nodes:
        chain = node['ancestors'] + [node['symbol']]
        root = next((s for s in chain if s.startswith('Thread_')), '')
        if 'com.apple.main-thread' not in root and 'Main Thread' not in root:
            continue
        if any('waitForProfiler(' in s for s in chain):
            continue
        own = node['self']
        main_samples += own
        if not own:
            continue
        # Match the nearest anonymous generated frame. Helper descendants retain
        # that calling region, but interpreter replay is reported separately.
        region = None
        for symbol in reversed(chain):
            if not symbol.startswith('???'):
                continue
            match = re.search(r'\[0x([0-9a-fA-F]+)\]', symbol)
            if match:
                region = code_map.lookup(int(match[1], 16))
            if region:
                break
        if region:
            mode = ('interpreter replay' if any('pc::CPU::step()' in s for s in chain)
                    else 'generated self' if node['symbol'].startswith('???')
                    else 'helper descendants')
            groups[(label(region), mode)] += own
            mapped += own
    return {'mainThreadTaskSamples': main_samples,
            'mappedSamples': mapped,
            'unmappedSamples': main_samples - mapped,
            'regions': [{'region': k[0], 'scope': k[1], 'samples': count,
                         'percentOfMain': 100 * count / main_samples if main_samples else 0}
                        for k, count in groups.most_common()]}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('sample', type=Path)
    p.add_argument('code_map', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    blocks = [json.loads(line) for line in a.code_map.read_text().splitlines()]
    try:
        validate_process(a.sample.read_text(), blocks)
    except ValueError as error:
        p.error(str(error))
    code_map = CodeMap(blocks)
    result = attribute(parse(a.sample), code_map)
    result.update({'sample': str(a.sample), 'codeMap': str(a.code_map),
                   'sampleSha256': hashlib.sha256(a.sample.read_bytes()).hexdigest(),
                   'codeMapSha256': hashlib.sha256(a.code_map.read_bytes()).hexdigest(),
                   'blocks': len(code_map.blocks),
                   'scope': 'Wall-stack samples, not exact CPU time. Helper samples inherit '
                            'their emitted caller region. Unmapped samples remain explicit.'})
    a.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
