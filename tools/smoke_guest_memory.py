#!/usr/bin/env python3
"""Check RAM memory sweep across engines and reject a corrupted guest kernel."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from bench_guest_memory import ROOT, validate

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qemu', action='store_true')
args = parser.parse_args()
work = Path(tempfile.mkdtemp(prefix='memory-smoke-', dir=ROOT / 'build'))
results = {}
for backend in ['jit', 'interpreter'] + (['qemu'] if args.qemu else []):
    subprocess.run(['python3', str(ROOT / 'tools/bench_guest_memory.py'),
                    '--backend', backend, '--loops', '257', '--output', str(work / backend)],
                   cwd=ROOT, check=True)
    results[backend] = json.loads((work / backend / 'result.json').read_text())
assert len({r['programSha256'] for r in results.values()}) == 1
assert len({r['guestTranscript'] for r in results.values()}) == 1
assert [s['guestInstructions'] for s in results['jit']['samples']] == [
    s['guestInstructions'] for s in results['interpreter']['samples']]
assert all(s['nativeInstructions'] > 0 for s in results['jit']['samples'])
assert all(s['nativeInstructions'] == 0 for s in results['interpreter']['samples'])
# Mutate one dependent load to two NOPs: timed work still completes, but exact
# pointer verification must reject it. Never call this a validated timing run.
blob = (work / 'jit/MEMORY.COM').read_bytes()
needle = bytes([0x8b, 0x00]) * 32
assert blob.count(needle) == 1
bad = work / 'bad.COM'
bad.write_bytes(blob.replace(needle, b'\x90\x90' + needle[2:]))
raw = work / 'bad.json'
subprocess.run([str(ROOT / 'build/memory-bench'), str(bad), str(raw), '--backend', 'jit'],
               check=True, timeout=30)
try:
    validate(Path(str(raw) + '.guest.txt').read_text(), 257)
except ValueError:
    pass
else:
    raise RuntimeError('Corrupted dependent-load kernel was accepted')
summary = {'scope': 'Short diagnostic sweeps, not benchmark timings',
           'engines': list(results), 'sameProgramAndResults': True,
           'jitInterpreterInstructionCountsMatch': True,
           'jitExecutesNatively': True, 'corruptedGuestRejected': True,
           'runs': results}
(work / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
print(f'PASS: matching guest results/instruction counts and corrupt-kernel rejection; {work}')
