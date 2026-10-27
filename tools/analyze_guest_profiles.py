#!/usr/bin/env python3
"""Aggregate interpreter coverage and diagnostic sampled CPU cost by leaf opcode."""
import argparse
import collections
import json
from pathlib import Path

TIMING_FIELDS = ('cpuLeafAttempts', 'cpuSamples', 'cpuRawNs', 'cpuAdjustedNs',
                 'cpuFaultSamples', 'cpuExceptionSamples')
NAMES = {0x63:'ARPL',0xcd:'INT imm8',0xab:'STOSW/D',0xe6:'OUT imm8,AL',0x8f:'POP rm',0x8b:'MOV r,rm',0x89:'MOV rm,r',0x83:'ALU rm,imm8',0x74:'JZ',0x75:'JNZ',
         0xff:'INC/DEC/CALL/JMP/PUSH group',0x3b:'CMP r,rm',0x39:'CMP rm,r',0x2b:'SUB r,rm',
         0xe8:'CALL relative',0xc3:'RET near',0xc2:'RET near imm',0xf6:'TEST/unary byte group',
         0xf7:'TEST/unary word/dword group',0xc1:'shift/rotate immediate',0xd0:'byte shift/rotate 1',
         0x8a:'MOV byte r,rm',0xf:'0F extended opcode count total',0x60:'PUSHA',0x61:'POPA',
         0x8e:'MOV segment',0x8c:'MOV from segment',0x9c:'PUSHF',0x9d:'POPF',0xfa:'CLI',0xfb:'STI',
         0xa4:'MOVSB',0xa5:'MOVSW/D',0xa6:'CMPSB',0xa7:'CMPSW/D',0xae:'SCASB',0xaf:'SCASW/D',
         0x1a4:'SHLD immediate',0x1a5:'SHLD CL',0x1ac:'SHRD immediate',0x1ad:'SHRD CL'}


def analyze(files):
    opcodes = collections.defaultdict(collections.Counter)
    groups = collections.defaultdict(collections.Counter)
    events = collections.Counter()
    unknown = collections.Counter()
    timing_files = []
    attempts = instructions = 0
    for file in files:
        s = json.loads(file.read_text())
        attempts += s['interpretedAttempts']
        instructions += s['stopInstructions'] - s['startInstructions']
        events.update(s['events'])
        timing = s.get('cpuTiming')
        if timing:
            if timing.get('version') != 1 or timing['clock'] != 'thread-cpu-ns':
                raise ValueError('Unsupported interpreter CPU timing schema')
            timing_files.append({'file': str(file), **timing})
            unknown.update({k: timing[k] for k in ('unknownAttempts', 'unknownSamples', 'unknownRawNs', 'unknownAdjustedNs')})
            if timing['unknownSamples'] > timing['unknownAttempts'] or timing['unknownAdjustedNs'] > timing['unknownRawNs']:
                raise ValueError('Invalid unknown CPU sample accounting')
        for op in s['opcodes']:
            code = op['opcode']
            values = {k: op.get(k, 0) for k in TIMING_FIELDS}
            if (any(v < 0 for v in values.values()) or values['cpuSamples'] > values['cpuLeafAttempts'] or
                    values['cpuLeafAttempts'] > op['attempts'] or values['cpuAdjustedNs'] > values['cpuRawNs'] or
                    values['cpuFaultSamples'] > values['cpuSamples'] or values['cpuExceptionSamples'] > values['cpuSamples']):
                raise ValueError('Invalid opcode CPU sample accounting')
            opcodes[code].update({k: op[k] for k in ('attempts', 'registerOperands', 'memoryOperands')})
            opcodes[code].update(values)
            groups[code].update(dict(enumerate(op['groups'])))
    if sum(c['attempts'] for op, c in opcodes.items() if op < 256) != attempts:
        raise ValueError('Primary opcode totals disagree with interpreted attempts')
    if attempts > instructions:
        raise ValueError('Interpreted attempts exceed guest instruction delta')
    rows = []
    unknown_estimate = (unknown['unknownAdjustedNs'] * unknown['unknownAttempts'] / unknown['unknownSamples']
                        if unknown['unknownSamples'] else 0)
    for code, counts in sorted(opcodes.items(), key=lambda x: -x[1]['attempts']):
        mean = counts['cpuAdjustedNs'] / counts['cpuSamples'] if counts['cpuSamples'] else 0
        rows.append({'opcode': f'{code:02x}' if code < 256 else f'0f {code-256:02x}',
                     'name': NAMES.get(code, ''), **counts,
                     'percentOfInterpretedAttempts': 100 * counts['attempts'] / attempts if attempts else 0,
                     'modRMGroups': dict(groups[code]), 'cpuMeanAdjustedNs': mean,
                     'cpuEstimatedNs': mean * counts['cpuLeafAttempts'],
                     'cpuLowSampleCount': counts['cpuSamples'] < 20})
    total = unknown_estimate + sum(row['cpuEstimatedNs'] for row in rows)
    for row in rows:
        row['cpuEstimatedPercent'] = 100 * row['cpuEstimatedNs'] / total if total else 0
    ranking = sorted((r for r in rows if r['cpuSamples']), key=lambda r: -r['cpuEstimatedNs'])
    return {'files': [str(f) for f in files], 'guestInstructions': instructions,
            'interpretedAttempts': attempts, 'events': events, 'opcodes': rows,
            'negativeHitPercentOfDispatch': 100 * events['negativeHit'] / events['dispatch'] if events['dispatch'] else 0,
            'cpuTiming': {'profiles': timing_files, 'available': any(p['available'] for p in timing_files),
                          'estimatedInterpreterNs': total, 'unknownEstimatedNs': unknown_estimate,
                          'unknownEstimatedPercent': 100 * unknown_estimate / total if total else 0,
                          'leafAttemptsWithoutSamples': sum(r['cpuLeafAttempts'] for r in rows if not r['cpuSamples']),
                          'opcodesByEstimatedCpu': ranking},
            'notes': ['0F primary and extended instruction counts overlap; do not sum both.',
                      'CPU timing uses only the final decoded opcode of an outer CPU::step scope; it does not double-count 0F.',
                      'Jittered CPU samples include snapshot/decode/execution/fault delivery and diagnostic counter overhead.',
                      'Adjusted CPU time subtracts a measured clock-pair mean after aggregating samples per opcode.',
                      'Estimated time extrapolates sampled mean to scoped leaf attempts, not aggregate opcode counts.',
                      'Unknown scopes include fetch faults or steps that decoded no opcode. Nested step cost stays in its outer scope.',
                      'Low sample counts and clock/counter overhead limit precision; this ranks diagnostic work, not release timings.',
                      'Operand/group counts include only instructions calling the ModRM decoder.',
                      'Attempt counts include faults and REP host-batch resumptions.',
                      'Disk read sectors include recursive parent reads; they are not physical IO bytes.',
                      'Instrumented executable timings are not performance measurements.']}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    files = sorted(a.directory.glob('run-*/guest-profile.json'))
    if not files:
        p.error('No guest profiles')
    report = analyze(files)
    a.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(files)} runs: {report["interpretedAttempts"]:,} interpreted attempts; {report["negativeHitPercentOfDispatch"]:.1f}% negative cache dispatches')
    for row in report['opcodes'][:15]:
        print(f'{row["opcode"]:>5} {row["percentOfInterpretedAttempts"]:5.2f}% {row["name"]}')
    if report['cpuTiming']['available']:
        print('Estimated interpreter CPU cost (sampling diagnostics, not benchmark scores):')
        for row in report['cpuTiming']['opcodesByEstimatedCpu'][:15]:
            print(f'{row["opcode"]:>5} {row["cpuEstimatedPercent"]:5.2f}% {row["cpuSamples"]:7} samples  {row["cpuMeanAdjustedNs"]:8.1f} ns/sample  {row["name"]}' + (' [few samples]' if row['cpuLowSampleCount'] else ''))
        print(f'Unknown/fetch/halted scopes: {report["cpuTiming"]["unknownEstimatedPercent"]:.2f}% estimated CPU')


if __name__ == '__main__':
    main()
