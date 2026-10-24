#!/usr/bin/env python3
"""Aggregate exact interpreter-attempt/operand and JIT-dispatch diagnostic counts."""
import argparse
import collections
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('directory', type=Path)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
files = sorted(a.directory.glob('run-*/guest-profile.json'))
if not files:
    p.error('No guest profiles')
opcodes = collections.defaultdict(collections.Counter)
groups = collections.defaultdict(collections.Counter)
events = collections.Counter()
attempts, instructions = 0, 0
for file in files:
    s = json.loads(file.read_text())
    attempts += s['interpretedAttempts']
    instructions += s['stopInstructions']-s['startInstructions']
    events.update(s['events'])
    for op in s['opcodes']:
        code = op['opcode']
        opcodes[code].update({k: op[k] for k in ('attempts', 'registerOperands', 'memoryOperands')})
        groups[code].update(dict(enumerate(op['groups'])))
if sum(c['attempts'] for op, c in opcodes.items() if op < 256) != attempts:
    raise ValueError('Primary opcode totals disagree with interpreted attempts')
if attempts > instructions:
    raise ValueError('Interpreted attempts exceed guest instruction delta')
names = {0x8b:'MOV r,rm',0x89:'MOV rm,r',0x83:'ALU rm,imm8',0x74:'JZ',0x75:'JNZ',
         0xff:'INC/DEC/CALL/JMP/PUSH group',0x3b:'CMP r,rm',0x39:'CMP rm,r',0x2b:'SUB r,rm',
         0xe8:'CALL relative',0xc3:'RET near',0xc2:'RET near imm',0xf6:'TEST/unary byte group',
         0xf7:'TEST/unary word/dword group',0xc1:'shift/rotate immediate',0xd0:'byte shift/rotate 1',
         0x8a:'MOV byte r,rm',0xf:'0F extended opcode total'}
rows = []
for code, counts in sorted(opcodes.items(), key=lambda x:-x[1]['attempts']):
    rows.append({'opcode':f'{code:02x}' if code<256 else f'0f {code-256:02x}',
                 'name':names.get(code,''),**counts,
                 'percentOfInterpretedAttempts':100*counts['attempts']/attempts,
                 'modRMGroups':dict(groups[code])})
report = {'files':[str(f) for f in files],'guestInstructions':instructions,
          'interpretedAttempts':attempts,'events':events,'opcodes':rows,
          'negativeHitPercentOfDispatch':100*events['negativeHit']/events['dispatch'],
          'notes':['0F primary and extended counts overlap; do not sum both.',
                   'Operand/group counts include only instructions calling the ModRM decoder.',
                   'Attempt counts include faulting instructions and REP host-batch resumptions.',
                   'Disk read sectors include recursive parent reads; they are not physical IO bytes.',
                   'Instrumented executable timings are not performance measurements.']}
a.output.write_text(json.dumps(report,indent=2)+'\n')
print(f'{len(files)} runs: {attempts:,} interpreted attempts; {report["negativeHitPercentOfDispatch"]:.1f}% negative cache dispatches')
for row in rows[:15]:
    print(f'{row["opcode"]:>5} {row["percentOfInterpretedAttempts"]:5.2f}% {row["name"]}')
