#!/usr/bin/env python3
"""Alternate independent DOS boots under JIT/interpreter; retain every raw sample.

CoreMark compares iterations per host second, not intervals with differing
calibrated iteration counts. Micro results stay separate by case and cache state.
"""
import argparse
import hashlib
import json
import math
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--suite', choices=['coremark', 'micro'], default='coremark')
p.add_argument('--repeat', type=int, default=3)
p.add_argument('--iterations', type=int, default=0, help='0: validated >=10s CoreMark; fixed short counts are diagnostic')
p.add_argument('--loops', type=int, default=5000000)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
if not 1 <= a.repeat <= 20 or not 0 <= a.iterations <= 0x7fffffff or not 1 <= a.loops <= 10000000:
    p.error('repeat must be 1..20, iterations 0..2147483647 and loops 1..10000000')
subprocess.run(['make', '-s', 'build/guest-bench', 'build/disk-tool'], cwd=root, check=True)
work = a.output.resolve()
work.mkdir(parents=True, exist_ok=False)
# One executable for all runs, even if an unrelated build occurs concurrently.
runner = work / 'guest-bench'
shutil.copy2(root / 'build/guest-bench', runner)
runner_hash = hashlib.sha256(runner.read_bytes()).hexdigest()
runs = []
program_hash = None
expected_micro = None

def distribution(values):
    return {'median': statistics.median(values), 'min': min(values), 'max': max(values),
            'samples': values}

for pair in range(a.repeat):
    order = ['jit', 'interpreter'] if pair % 2 == 0 else ['interpreter', 'jit']
    for engine in order:
        target = work / f'pair-{pair}-{engine}'
        command = [sys.executable, str(root / 'tools/bench_guest_perf.py'), '--suite', a.suite,
                   '--iterations', str(a.iterations), '--loops', str(a.loops),
                   '--output', str(target), '--runner', str(runner)]
        if engine == 'interpreter':
            command.append('--no-jit')
        subprocess.run(command, cwd=root, check=True)
        data = json.loads((target / 'result.json').read_text())
        digest = hashlib.sha256((target / 'program/PCBENCH.COM').read_bytes()).hexdigest()
        if program_hash is not None and program_hash != digest:
            raise RuntimeError('Guest program changed between runs; comparison refused')
        program_hash = digest
        if data['runnerSha256'] != runner_hash or not data['returnedToDos']:
            raise RuntimeError('Runner identity or DOS return validation failed')
        if a.suite == 'coremark':
            if not data['crcValidated'] or (not a.iterations and not data['publishedScoreEligible']):
                raise RuntimeError('CoreMark validation failed')
            rows = [{'case': 'coremark', 'cache': 'calibrated-warm' if not a.iterations else 'fixed-iterations',
                     'rate': data['iterationsPerHostSecond'], 'unit': 'iterations/host-second'}]
        else:
            if not data['microValidated'] or len(data['samples']) != 32:
                raise RuntimeError('Micro matrix validation failed')
            signature = [(s['case'], s['cache'], s['checksum'], s['guestInstructions'],
                          s['workUnits'], s['workUnit']) for s in data['samples']]
            if expected_micro is not None and signature != expected_micro:
                raise RuntimeError('Guest results or instruction counts changed between runs')
            expected_micro = signature
            rows = [{'case': s['case'], 'name': s['name'], 'cache': s['cache'],
                     'rate': s['unitsPerHostSecond'], 'unit': s['workUnit']+'/host-second'}
                    for s in data['samples']]
        if any(not math.isfinite(row['rate']) or row['rate'] <= 0 for row in rows):
            raise RuntimeError('Invalid host throughput')
        runs.append({'pair': pair, 'engine': engine, 'rawResult': str(target / 'result.json'),
                     'rows': rows})
        (work / 'runs.json').write_text(json.dumps(runs, indent=2)+'\n')
        print(f'Validated pair {pair+1}/{a.repeat}: {engine}', flush=True)
summary = []
for initial in runs[0]['rows']:
    rates = {engine: [] for engine in ['jit', 'interpreter']}
    for run in runs:
        row = next(row for row in run['rows'] if (row['case'], row['cache']) ==
                   (initial['case'], initial['cache']))
        rates[run['engine']].append(row['rate'])
    summary.append({'case': initial['case'], 'cache': initial['cache'], 'unit': initial['unit'],
                    'jit': distribution(rates['jit']), 'interpreter': distribution(rates['interpreter']),
                    'pairedSpeedup': distribution([j/r for j, r in zip(rates['jit'], rates['interpreter'])])})
report = {'suite': a.suite, 'repeat': a.repeat, 'runnerSha256': runner_hash,
          'programSha256': program_hash, 'scope': 'headless guest CPU workload; not application or Rosetta performance',
          'coremarkScoresEligible': a.suite == 'coremark' and not a.iterations,
          'order': [{'pair': r['pair'], 'engine': r['engine']} for r in runs], 'summary': summary}
(work / 'result.json').write_text(json.dumps(report, indent=2)+'\n')
print(f'PASS: {a.repeat} alternating pairs; all raw runs retained at {work}', flush=True)
