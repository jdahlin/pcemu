#!/usr/bin/env python3
"""Aggregate macOS sample call trees without double-counting inclusive frames."""
import argparse
import collections
import json
import re
from pathlib import Path


def parse(path):
    text = path.read_text()
    tree = text.split('Call graph:\n', 1)[1].split('Total number in stack', 1)[0]
    stack, nodes = [], []
    for line in tree.splitlines():
        m = re.match(r'^([ +!:\|`-]*)(\d+) (.+)$', line)
        if not m:
            continue
        depth, count, symbol = len(m[1]), int(m[2]), m[3]
        while stack and stack[-1]['depth'] >= depth:
            stack.pop()
        node = {'depth': depth, 'count': count, 'self': count, 'symbol': symbol,
                'ancestors': [n['symbol'] for n in stack]}
        if stack:
            stack[-1]['self'] -= count
        stack.append(node)
        nodes.append(node)
    if any(n['self'] < 0 for n in nodes):
        raise ValueError(f'Inconsistent call tree in {path}')
    return nodes


def category(node):
    chain = '\n'.join(node['ancestors'] + [node['symbol']])
    if 'waitForProfiler(' in chain:
        return 'Profiler attachment barrier (excluded)'
    if 'pc::Jit::compile(' in chain:
        return 'JIT compilation (including decode exceptions)'
    if 'pc::Disk::' in chain or 'ZSTD_' in chain:
        return 'Disk and compression'
    if 'pc::Machine::poll()' in chain:
        return 'Device polling and interrupt delivery'
    if 'pc::CPU::step()' in chain:
        return 'Interpreter (including MMU and guest faults)'
    if 'pc::Jit::run()' in chain:
        if '???' in chain or 'pc::native' in chain:
            return 'Generated code and native helpers'
        return 'JIT dispatch/cache/validation'
    if 'SDL_' in chain or 'SDL2' in chain:
        return 'SDL rendering and window events'
    if 'pc::VGA::pixel(' in chain or 'main::$_' in chain or '__write' in chain:
        return 'Capture/video/control output'
    return 'Main loop/other host work'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    files = sorted(a.directory.glob('run-*/sample.txt'))
    if not files:
        p.error('No sample files')
    groups, self_functions, inclusive_functions = (collections.Counter() for _ in range(3))
    total = 0
    excluded = 0
    workers = collections.defaultdict(collections.Counter)
    for path in files:
        for n in parse(path):
            own = n['self']
            root = next((s for s in n['ancestors'] + [n['symbol']] if s.startswith('Thread_')), '')
            if 'com.apple.main-thread' not in root and 'Main Thread' not in root:
                blocking = any(s in n['symbol'] for s in ('__workq_kernreturn', 'mach_msg',
                    '__psynch_cvwait', 'kevent', 'semaphore_wait', 'nanosleep', '__psynch_mutexwait'))
                workers[root]['blockedSamples' if blocking else 'otherSamples'] += own
                continue
            if category(n) == 'Profiler attachment barrier (excluded)':
                excluded += own
                continue
            total += own
            groups[category(n)] += own
            function = n['symbol'].split('  (in ', 1)[0]
            self_functions[function] += own
            # One inclusive count per function on each leaf's ancestry, even
            # when recursion or compiler-expanded inline frames repeat it.
            for f in set(s.split('  (in ', 1)[0] for s in n['ancestors'] + [n['symbol']]):
                inclusive_functions[f] += own
    def rows(counter):
        return [{'name': k, 'samples': n, 'percent': 100*n/total}
                for k, n in counter.most_common() if n]
    if not total:
        p.error('No task samples: sampler attachment did not finish before process exit')
    report = {'files': [str(f) for f in files], 'samples': total,'attachmentBarrierSamplesExcluded':excluded,
              'workerThreads':dict(workers),
              'scope': 'main-thread wall-stack samples; attachment barrier excluded; worker blocking stacks reported separately; small attachment/exit margins remain; profiled timings are not benchmarks',
              'categories': rows(groups), 'selfFunctions': rows(self_functions),
              'inclusiveFunctions': rows(inclusive_functions)}
    a.output.write_text(json.dumps(report, indent=2)+'\n')
    print(f'{len(files)} traces, {total} samples')
    for row in report['categories']:
        print(f"{row['percent']:5.1f}% {row['name']}")


if __name__ == '__main__':
    main()
