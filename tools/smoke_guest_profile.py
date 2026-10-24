#!/usr/bin/env python3
"""Validate interval-scoped guest profiles against real DOS benchmark results."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parent.parent
work = Path(tempfile.mkdtemp(prefix='guest-profile-smoke-', dir=root / 'build'))
rejected = subprocess.run([str(root / 'build/guest-bench'), '--guest-profile'],
                          capture_output=True, text=True)
assert rejected.returncode and 'requires build/guest-bench-profile' in rejected.stderr
results = {}
for suite in ('coremark', 'micro'):
    signature = None
    for engine in ('jit', 'interpreter'):
        target = work / (suite + '-' + engine)
        command = [sys.executable, str(root / 'tools/bench_guest_perf.py'),
                   '--guest-profile', '--suite', suite, '--iterations', '10',
                   '--loops', '17', '--output', str(target)]
        if engine == 'interpreter':
            command.append('--no-jit')
        subprocess.run(command, cwd=root, check=True)
        data = json.loads((target / 'result.json').read_text())
        assert data['profiled'] and data['returnedToDos']
        assert not data.get('publishedScoreEligible', False)
        assert data['crcValidated' if suite == 'coremark' else 'microValidated']
        assert not (target / 'disk.pcd').exists()
        current = []
        for sample in data['samples']:
            counters = json.loads(Path(sample['guestProfile']).read_text())
            assert counters['stopInstructions'] - counters['startInstructions'] == sample['guestInstructions']
            assert counters['interpretedAttempts'] == sample['guestInstructions'] - sample['nativeInstructions']
            assert counters['events']['interpretedDispatch'] == counters['interpretedAttempts']
            assert counters['events']['dispatch'] == (counters['events']['nativeCall'] +
                                                      counters['events']['interpretedDispatch'])
            assert counters['opcodes'] and counters['sampledIPs']
            current.append((sample['case'], sample['guestInstructions'], sample['checksum']))
        if signature is not None:
            assert current == signature, 'JIT/interpreter guest work differs'
        signature = current
        results[suite + '-' + engine] = str(target / 'result.json')
(work / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
print('PASS: interval counters, guest checksums, JIT/interpreter work and diagnostic timing labels')
print('Evidence:', work)
