#!/usr/bin/env python3
"""Optional media-based validation of guest benchmarks and rejection of a wrong result."""
import json
import subprocess
import tempfile
from pathlib import Path
from cow_file import CowFile
from fat_stage import Writer

ROOT = Path(__file__).resolve().parent.parent
work = Path(tempfile.mkdtemp(prefix='guest-perf-smoke-', dir=ROOT / 'build'))
for suite in ['coremark', 'micro']:
    for interpreter in [False, True]:
        directory = work / (suite + ('-ref' if interpreter else '-jit'))
        command = ['python3', str(ROOT / 'tools/bench_guest_perf.py'), '--suite', suite,
                   '--iterations', '10', '--loops', '17', '--output', str(directory)]
        if interpreter:
            command.append('--no-jit')
        subprocess.run(command, cwd=ROOT, check=True)
        data = json.loads((directory / 'result.json').read_text())
        if not data['returnedToDos']:
            raise RuntimeError('Guest failed to return to DOS')
        if suite == 'coremark':
            if not data['crcValidated'] or data['upstreamValidated'] or data['publishedScoreEligible']:
                raise RuntimeError('Short CoreMark diagnostic was misclassified')
        else:
            if not data['microValidated'] or len(data['samples']) != 24:
                raise RuntimeError('Microbenchmark matrix missing')
    if suite == 'micro':
        jit = json.loads((work / 'micro-jit/result.json').read_text())
        ref = json.loads((work / 'micro-ref/result.json').read_text())
        for actual, expected in zip(jit['samples'], ref['samples']):
            if ((actual['case'], actual['checksum'], actual['guestInstructions']) !=
                    (expected['case'], expected['checksum'], expected['guestInstructions'])):
                raise RuntimeError('Interpreter and JIT work/results disagree')

# Original real-mode probe: select ALU16, begin/end, then report zero instead
# of the required 17 * 48 = 816. A fast but wrong guest must fail validation.
bad = bytes.fromhex('b0 10 e6 e6 b0 01 e6 e7 b0 02 e6 e7 66 31 c0 66 e7 e5 b8 00 4c cd 21')
disk = work / 'bad-result.pcd'
subprocess.run([str(ROOT / 'build/disk-tool'), 'overlay', str(disk),
                str(ROOT / 'media/dos.raw')], check=True)
with CowFile(disk) as handle:
    fat = Writer(handle)
    fat.root_entry(fat.entry('PCBENCH.COM', 32, fat.allocate(bad), len(bad)))
    fat.commit()
    fat.replace_existing('CONFIG  SYS', b'FILES=30\r\nBUFFERS=20\r\n')
    fat.replace_existing('AUTOEXECBAT', b'@ECHO OFF\r\nPCBENCH.COM\r\n'
                         b'ECHO PCEMU benchmark returned to DOS\r\n')
result = subprocess.run([str(ROOT / 'build/guest-bench'), '--disk', str(disk),
                         '--loops', '17', '--output', str(work / 'bad-result.json')],
                        capture_output=True, text=True, timeout=30)
(work / 'bad-result.log').write_text(result.stdout + result.stderr)
if result.returncode == 0 or 'checksum 0 expected 816' not in result.stderr:
    raise RuntimeError('Wrong guest result was not rejected; see ' + str(work))
disk.unlink()
(work / 'result.json').write_text(json.dumps({
    'coremarkCrcBothEngines': True, 'shortScoresRejected': True,
    'microIntervalsBothEngines': 48, 'microResultsAndInstructionCountsMatch': True,
    'wrongGuestChecksumRejected': True,
}, indent=2) + '\n')
print('PASS: guest benchmark CRCs, cold/warm matrix, instruction counts and wrong-result rejection')
print('Evidence: ' + str(work))
