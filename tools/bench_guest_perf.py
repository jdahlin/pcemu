#!/usr/bin/env python3
"""Boot CoreMark or 16/32-bit microkernels in DOS and retain validated host-timed results."""
import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import tempfile
from pathlib import Path
from cow_file import CowFile
from fat_stage import Writer

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--suite', choices=['coremark', 'micro'], default='coremark')
parser.add_argument('--loops', type=int, default=1000000)
parser.add_argument('--iterations', type=int, default=0,
                    help='0: upstream automatic >=10s run; short fixed counts are diagnostic only')
parser.add_argument('--no-jit', action='store_true')
parser.add_argument('--timeout', type=int, default=180)
parser.add_argument('--output', type=Path)
parser.add_argument('--runner', type=Path, help='use an existing guest-bench executable without rebuilding it')
args = parser.parse_args()
if not 1 <= args.loops <= 10000000:
    parser.error('loops must be 1..10000000')
if not 0 <= args.iterations <= 0x7fffffff:
    parser.error('iterations outside signed 32-bit range')
if args.timeout <= 0:
    parser.error('timeout must be positive')
runner = args.runner.resolve() if args.runner else ROOT / 'build/guest-bench'
if args.runner and (not runner.is_file() or not os.access(runner, os.X_OK)):
    parser.error('Runner must be an existing executable')
targets = ['build/disk-tool'] + ([] if args.runner else ['build/guest-bench'])
subprocess.run(['make', '-s', *targets], cwd=ROOT, check=True)
work = (args.output.resolve() if args.output else
        Path(tempfile.mkdtemp(prefix='guest-perf-', dir=ROOT / 'build')))
work.mkdir(parents=True, exist_ok=True)
disk = work / 'disk.pcd'
if disk.exists():
    parser.error('Destination disk already exists; choose a new output directory')
subprocess.run(['python3', str(ROOT / 'tools/build_guest_perf.py'), '--suite', args.suite,
                '--iterations', str(args.iterations), '--output', str(work / 'program')], check=True)
subprocess.run([str(ROOT / 'build/disk-tool'), 'overlay', str(disk),
                str(ROOT / 'media/dos.raw')], check=True)
with CowFile(disk) as handle:
    fat = Writer(handle)
    program = (work / 'program/PCBENCH.COM').read_bytes()
    fat.root_entry(fat.entry('PCBENCH.COM', 32, fat.allocate(program), len(program)))
    fat.commit()
    fat.replace_existing('CONFIG  SYS', b'FILES=30\r\nBUFFERS=20\r\n')
    fat.replace_existing('AUTOEXECBAT', b'@ECHO OFF\r\nPCBENCH.COM\r\n'
                         b'ECHO PCEMU benchmark returned to DOS\r\n')
    entry = next(entry for entry in fat.entries() if entry[1] == 'PCBENCH COM')
    if fat.read_file(entry[2], entry[3]) != program:
        raise ValueError('Staged COM readback mismatch')
result = work / 'result.json'
provenance = {
    'runnerSha256': hashlib.sha256(runner.read_bytes()).hexdigest(),
    'runnerPath': str(runner),
    'gitCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
    'gitDirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
    'host': {'system': platform.system(), 'release': platform.release(),
             'machine': platform.machine(), 'logicalCpus': os.cpu_count(),
             'loadAverageBefore': os.getloadavg()},
}
command = [str(runner), '--disk', str(disk), '--output', str(result),
           '--timeout', str(args.timeout), '--loops', str(args.loops)]
if args.no_jit:
    command.append('--no-jit')
with (work / 'run.log').open('w') as log:
    subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                   check=True, timeout=args.timeout + 10)
data = json.loads(result.read_text())
data.update(provenance)
data['host']['loadAverageAfter'] = os.getloadavg()
data['build'] = json.loads((work / 'program/build.json').read_text())
transcript = Path(str(result) + '.guest.txt').read_text()
if args.suite == 'coremark':
    iterations = int(re.search(r'Iterations\s+: (\d+)', transcript).group(1))
    data['iterations'] = iterations
    data['iterationsPerHostSecond'] = iterations / data['samples'][-1]['hostSeconds']
    data['publishedScoreEligible'] = (data['upstreamValidated'] and
                                     data['samples'][-1]['hostSeconds'] >= 10)
    print(f"PASS: {data['iterationsPerHostSecond']:.3f} iterations/host second; CRC checked; returned to DOS; "
          + ('>=10s upstream validation passed' if data['publishedScoreEligible'] else
             'short diagnostic, not a publishable CoreMark score'))
else:
    if not data['microValidated'] or len(data['samples']) != 28:
        raise RuntimeError('Microbenchmark matrix was not validated')
    names = ['alu', 'memory-update', 'branch', 'call-ret', 'rep-copy', 'loop-counter', 'byte-exchange']
    for row in data['samples']:
        index = row['case'] - (16 if row['case'] < 32 else 32)
        row['name'] = names[index]
        row['operandBits'] = 16 if row['case'] < 32 else 32
        row['codeBits'] = row['operandBits']
        if row['case'] in (22, 38):
            row['operandBits'] = 8
            row['accumulatorBits'] = 16
            row['counterBits'] = 32
        row['cache'] = 'cold' if row['jitClearedBefore'] else 'warm'
        row['workUnits'] = [16 * args.loops, 8 * args.loops, args.loops, 4 * args.loops,
                            max(1, args.loops // 64) *
                            (128 if row['operandBits'] == 16 else 256), args.loops, 2 * args.loops][index]
        row['workUnit'] = ['additions', 'updates', 'iterations', 'calls', 'bytes', 'iterations', 'exchanges'][index]
        row['unitsPerHostSecond'] = row['workUnits'] / row['hostSeconds']
    print('PASS: 28 cold/warm 16/32-bit microbenchmark intervals, every checksum checked; returned to DOS')
result.write_text(json.dumps(data, indent=2) + '\n')
print(f'Evidence: {work}')
disk.unlink()  # Only this private compressed child; original media is unchanged.
