#!/usr/bin/env python3
"""Summarize Instruments Time Profiler samples inside verified guest-task windows."""
import argparse
from collections import Counter, defaultdict
from datetime import datetime
import json
import re
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

from analyze_samples import category
from jit_debug_map import TimedCodeMap


def resolved(element, by_id):
    return by_id[element.attrib['ref']] if 'ref' in element.attrib else element


def trace_start(trace):
    toc = ET.fromstring(subprocess.check_output(
        ['xcrun', 'xctrace', 'export', '--input', str(trace), '--toc']))
    value = toc.findtext('./run/info/summary/start-date')
    if not value:
        raise ValueError(f'No Instruments start date in {trace}')
    return datetime.fromisoformat(value)


def trace_rows(trace, xml):
    if not xml.exists():
        subprocess.run(['xcrun', 'xctrace', 'export', '--input', str(trace),
                        '--xpath', '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]',
                        '--output', str(xml)], check=True, stdout=subprocess.DEVNULL)
    root = ET.parse(xml).getroot()
    ids = {element.attrib['id']: element for element in root.iter() if 'id' in element.attrib}
    for row in root.findall('.//row'):
        time_ns = int(resolved(row.find('sample-time'), ids).text)
        thread = resolved(row.find('thread'), ids).attrib.get('fmt', '')
        if row.find('tagged-backtrace') is None:
            continue
        stack = resolved(row.find('tagged-backtrace'), ids)
        backtrace = resolved(stack.find('backtrace'), ids)
        frames = [resolved(frame, ids) for frame in backtrace.findall('frame')]
        yield time_ns, thread, frames, ids


def site(frame, ids):
    name = frame.attrib.get('name', '???')
    source = frame.find('source')
    if source is None or source.attrib.get('line') == '0':
        return name
    path = resolved(source.find('path'), ids).text
    return f'{name} [{Path(path).name}:{source.attrib["line"]}]'


def analyze(report, directory):
    groups, sites, functions = Counter(), Counter(), Counter()
    quartiles = [Counter() for _ in range(4)]
    jit_regions, jit_blocks = Counter(), Counter()
    unmapped_jit = 0
    outside, workers, total = 0, defaultdict(Counter), 0
    runs = []
    for index, sample in enumerate(report['samples']):
        trace = directory / f'run-{index}' / 'instruments.trace'
        xml = trace.parent / 'time-profile.xml'
        origin = trace_start(trace)
        origin_ns = int(origin.timestamp() * 1e9)
        code_map = TimedCodeMap(trace.parent/'jit-debug.jsonl') if report.get('jitDebugMapped') else None
        start = int((datetime.fromisoformat(sample['taskStartUtc']) - origin).total_seconds() * 1e9)
        end = int((datetime.fromisoformat(sample['taskEndUtc']) - origin).total_seconds() * 1e9)
        if not 0 <= start < end:
            raise ValueError(f'Task window outside trace: {trace}')
        run = Counter()
        for timestamp, thread, frames, ids in trace_rows(trace, xml):
            if not start <= timestamp <= end:
                outside += 1
                continue
            if not frames:
                continue
            names = [frame.attrib.get('name', '???') for frame in frames]
            if code_map:
                pid = re.search(r'pid: (\d+)', thread)
                if not pid or int(pid[1]) != code_map.pid:
                    raise ValueError('Instruments process and JIT map PID differ')
            if 'Main Thread' not in thread:
                workers[thread][names[0]] += 1
                continue
            leaf = {'symbol': names[0], 'ancestors': names[:0:-1]}
            group = category(leaf)
            groups[group] += 1
            run[group] += 1
            quartiles[min(3, 4 * (timestamp - start) // (end - start))][group] += 1
            sites[site(frames[0], ids)] += 1
            functions[names[0]] += 1
            total += 1
            if code_map:
                found = None
                for depth, frame in enumerate(frames):
                    if not frame.attrib.get('name', '').startswith(('0x', '???')):
                        continue
                    # Instruments tags ARM64 PCs with low bits. Caller frames
                    # point after BL/BLR: attribute the call instruction itself.
                    address = int(frame.attrib['addr'], 16) & ~3
                    found = code_map.lookup(address - (4 if depth else 0), origin_ns + timestamp)
                    if found:
                        block, region = found
                        scope = 'generated self' if depth == 0 else 'helper descendants'
                        jit_regions[(region['kind'], scope)] += 1
                        jit_blocks[(block['cr3'], block['cs'], block['ip'])] += 1
                        break
                if not found and names[0].startswith(('0x', '???')):
                    unmapped_jit += 1
        runs.append({'trace': str(trace), 'windowStartNs': start, 'windowEndNs': end,
                     'mainThreadSamples': sum(run.values()), 'categories': dict(run)})
    if not total:
        raise ValueError('No main-thread samples inside task windows')
    def rows(counter):
        return [{'name': name, 'samples': count, 'percent': 100 * count / total}
                for name, count in counter.most_common()]
    return {'scope': 'Instruments Time Profiler CPU samples on the main thread, filtered to host task-command through verified-pixel response UTC timestamps; capture and control work inside this window remain included. Sample shares are not exact function CPU durations.',
            'samples': total, 'outsideTaskSamples': outside,
            'workerSamples': {thread: sum(counts.values()) for thread, counts in workers.items()},
            'runs': runs, 'categories': rows(groups),
            'jitRegions': [{'region': key[0], 'scope': key[1], 'samples': count,
                            'percentOfMain': 100 * count / total}
                           for key, count in jit_regions.most_common()],
            'jitBlocks': [{'cr3': key[0], 'cs': key[1], 'ip': key[2], 'samples': count}
                          for key, count in jit_blocks.most_common(30)],
            'unmappedJitSelfSamples': unmapped_jit,
            'timeQuartiles': [{'index': index + 1, 'samples': sum(counts.values()),
                               'categories': dict(counts)}
                              for index, counts in enumerate(quartiles)],
            'selfSites': rows(sites), 'selfFunctions': rows(functions)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    report = json.loads((args.directory / 'result.json').read_text())
    if not report.get('instrumentsProfiled'):
        p.error('Benchmark report has no Instruments recording')
    result = analyze(report, args.directory)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f"{len(result['runs'])} traces, {result['samples']} main-thread task samples")
    for row in result['categories']:
        print(f"{row['percent']:5.1f}% {row['name']}")


if __name__ == '__main__':
    main()
