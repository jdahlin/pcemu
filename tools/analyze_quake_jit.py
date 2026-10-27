#!/usr/bin/env python3
"""Attribute exact Quake frame-window CPU samples, respecting JIT address lifetimes."""
import collections
import re
from analyze_instruments import trace_start, trace_rows, site
from jit_debug_map import TimedCodeMap


def category(names, mapped_leaf=False):
    chain = '\n'.join(names)
    if any(name in chain for name in ('pc::Jit::debugBlock(', 'pc::Jit::flushDebugMap(', 'pc::Jit::debugReset(')):
        return 'Diagnostic code-map bookkeeping'
    if 'pc::Jit::compile(' in chain:
        return 'Compilation and decoding'
    if 'pc::CPU::step()' in chain:
        return 'Interpreter and guest faults'
    families = [
        ('Compiled x87 helpers', ('pc::CPU::stepX87(', 'pc::CPU::execute_x87_decoded(')),
        ('Compiled string helpers', ('pc::CPU::stepString(', 'pc::CPU::execute_string(', 'pc::jit_detail::nativeString(')),
        ('Compiled memory arithmetic helpers', ('pc::CPU::stepMemoryArithmetic(', 'pc::CPU::execute_group3(')),
        ('Compiled segment/interrupt/I-O helpers', ('pc::CPU::stepSegment(', 'pc::CPU::stepInterrupt(', 'pc::CPU::stepPortIO(')),
        ('Flags helpers', ('pc::jit_detail::nativeFinish(',)),
    ]
    for label, functions in families:
        if any(function in chain for function in functions): return label
    if any(name in names[0] for name in ('mach_continuous_time', 'clock_gettime', 'steady_clock::now')):
        return 'Host clock reads (PIT/runner)'
    if 'pc::Machine::poll()' in chain:
        return 'Device polling and IRQ delivery'
    if 'pc::jit_detail::native' in chain or 'pc::nativeMemory(' in chain:
        return 'Other compiled runtime helpers'
    if names[0].startswith(('0x', '???')):
        return 'Generated code' if mapped_leaf else 'Unmapped generated code'
    if any('pc::Jit::' in name for name in names):
        return 'Dispatch/cache/fetch validation'
    return 'Other host work'


def analyze(directory, window, pid):
    trace = directory / 'instruments.trace'
    origin = trace_start(trace)
    # xctrace's origin is not necessarily more precise than milliseconds;
    # TimedCodeMap rejects samples near load/recycle boundaries conservatively.
    origin_ns = int(origin.timestamp() * 1e9)
    code = TimedCodeMap(directory / 'jit-debug.jsonl')
    if code.pid != pid: raise ValueError('JIT map and benchmark PID differ')
    start, end = window['beginUnixNs'], window['endUnixNs']
    if not 0 < start < end: raise ValueError('Invalid Quake host sampling window')
    groups, functions, sites, regions, blocks = (collections.Counter() for _ in range(5))
    outside = workers = total = unresolved = 0
    for stamp, thread, frames, ids in trace_rows(trace, directory / 'time-profile.xml'):
        absolute = origin_ns + stamp
        if not start <= absolute <= end:
            outside += 1; continue
        match = re.search(r'pid: (\d+)', thread)
        if not match or int(match[1]) != pid: raise ValueError('Trace and benchmark PID differ')
        if 'Main Thread' not in thread:
            workers += 1; continue
        if not frames: continue
        names = [frame.attrib.get('name', '???') for frame in frames]
        leaf_mapped = False
        for depth, frame in enumerate(frames):
            if not frame.attrib.get('name', '').startswith(('0x', '???')): continue
            address = int(frame.attrib['addr'], 16) & ~3
            found = code.lookup(address - (4 if depth else 0), absolute)
            if found:
                block, region = found
                regions[(region['kind'], 'self' if depth == 0 else 'helper descendants')] += 1
                blocks[(block['cr3'], block['cs'], block['ip'])] += 1
                leaf_mapped = depth == 0
                break
        if names[0].startswith(('0x', '???')) and not leaf_mapped: unresolved += 1
        groups[category(names, leaf_mapped)] += 1
        functions[names[0]] += 1; sites[site(frames[0], ids)] += 1; total += 1
    if not total: raise ValueError('No main-thread CPU samples in Quake frame window')
    assert sum(groups.values()) == total
    def rows(counter):
        return [dict(name=name, samples=count, percentOfMainThread=100*count/total)
                for name, count in counter.most_common()]
    return dict(mainThreadSamples=total, outsideWindowSamples=outside, workerSamples=workers,
                unmappedGeneratedSelfSamples=unresolved, categories=rows(groups), self=rows(functions), sites=rows(sites),
                jitRegions=[dict(region=key[0], scope=key[1], samples=count) for key,count in regions.most_common()],
                jitBlocks=[dict(cr3=key[0], cs=key[1], ip=key[2], samples=count) for key,count in blocks.most_common(30)],
                notes=['Exclusive CPU sample categories conserve the window total; these are not exact function durations.',
                       'Typed helpers run outside generated frames and are identified by explicit symbol ancestry.',
                       'Unmapped PCs and publication/recycle uncertainty are retained, never assigned to a guessed block.'])
