#!/usr/bin/env python3
"""Check bounded Win95 startup profiling against the uninstrumented guest state."""
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix='startup-profile-', dir=root / 'build'))
rejected = subprocess.run([str(root / 'build/pcemu'), '--guest-profile', str(work / 'invalid.json')],
                          capture_output=True, text=True)
assert rejected.returncode and 'requires build/pcemu-profile' in rejected.stderr
assert not (work / 'invalid.json').exists()
states, runs = [], []
for name in ('pcemu', 'pcemu-profile'):
    directory = work / name
    directory.mkdir()
    disk = directory / 'disk.pcd'
    subprocess.run([str(root / 'build/disk-tool'), 'overlay', str(disk),
                    str(root / 'media/rtts-win95.img')], check=True)
    executable = root / 'build' / name
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    command = [str(executable), '--headless', '--machine', 'win95', '--ram-mb', '64',
               '--execute-vgabios', '--no-network', '--steps', '300000000',
               '--disk', str(disk), '--state-json', str(directory / 'state.json')]
    if name == 'pcemu-profile':
        command += ['--guest-profile', str(directory / 'guest-profile.json')]
    with (directory / 'guest.log').open('w') as log:
        subprocess.run(command, cwd=root, stdout=log, stderr=log, check=True, timeout=180)
    assert digest == hashlib.sha256(executable.read_bytes()).hexdigest()
    state = json.loads((directory / 'state.json').read_text())
    states.append({k: v for k, v in state.items() if k not in ('disk', 'executionWallNs')})
    runs.append({'command': command, 'executableSha256': digest,
                 'privateAllocatedBytes': disk.stat().st_blocks * 512})
    if name == 'pcemu-profile':
        profile = json.loads((directory / 'guest-profile.json').read_text())
        assert 0 <= profile['startInstructions'] < profile['stopInstructions']
        assert profile['stopInstructions'] == state['instructions']
        assert sum(row['attempts'] for row in profile['opcodes'] if row['opcode'] < 256) == profile['interpretedAttempts']
        # Faulting dispatch can exit before selecting either execution path.
        assert profile['events']['dispatch'] >= profile['events']['nativeCall'] + profile['events']['interpretedDispatch']
        assert profile['interpretedAttempts'] == profile['events']['interpretedDispatch'] + profile['events']['nativeReplay']
    # Preserve a failed run for diagnosis; successful children are removed below.
assert states[0] == states[1], 'Diagnostic instrumentation changed captured guest/device state'
for name in ('pcemu', 'pcemu-profile'):
    (work / name / 'disk.pcd').unlink()
result = {'scope': '300M instruction startup prefix; captured guest/device and JIT counts match; not full boot or bytewise RAM equality; instrumented timings are not benchmarks',
          'runs': runs, 'state': states[0], 'profile': profile, 'privateDisksRemoved': True}
(work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print('PASS: bounded profile accounting and normal/profile captured states match:', work)
for row in sorted(profile['opcodes'], key=lambda row: -row['attempts'])[:15]:
    print(hex(row['opcode']), row['attempts'], 'reg/memory', row['registerOperands'], row['memoryOperands'])
