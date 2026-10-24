#!/usr/bin/env python3
"""Cold-boot Win95 from a private compressed overlay and verify Explorer pixels.

Requires local installation media/images and Tesseract. Optional guest restart
checks reset delivery and a second desktop. A failed run retains its owned guest
and disk for diagnosis; successful runs remove only their private disk.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--disk', type=Path, default=root / 'media/rtts-win95.img')
parser.add_argument('--timeout', type=float, default=240,
                    help='seconds per desktop/reset wait')
parser.add_argument('--sample', action='store_true',
                    help='collect macOS host stacks through desktop verification; timings include profiling/OCR overhead')
parser.add_argument('--restart', action='store_true', help='also verify a guest-requested restart')
jump_mode = parser.add_mutually_exclusive_group()
jump_mode.add_argument('--jit-follow-jumps', action='store_true')
jump_mode.add_argument('--jit-traces', action='store_true')
args = parser.parse_args()
if args.timeout <= 0:
    parser.error('timeout must be positive')
if args.sample and not shutil.which('sample'):
    parser.error('macOS sample tool is unavailable')
source = args.disk.resolve()
if not source.is_file():
    parser.error('Missing local source disk: ' + str(source))
if not shutil.which('tesseract'):
    parser.error('This optional integration check requires tesseract (brew install tesseract)')
subprocess.run(['make', '-s', 'build/pcemu', 'build/disk-tool'], cwd=root, check=True)
work = Path(tempfile.mkdtemp(prefix='win95-smoke-', dir=root / 'build'))
disk, control, frame, state = (work / name for name in
                              ['disk.pcd', 'control.txt', 'frame.ppm', 'state'])
subprocess.run([str(root / 'build/disk-tool'), 'overlay', str(disk), str(source)], check=True)
control.write_text('profile-wait 3000\n' if args.sample else '')
emulator_hash = hashlib.sha256((root / 'build/pcemu').read_bytes()).hexdigest()
sampler = None
sampler_log = None
profile_dir = work / 'host-profile'
if args.sample:
    (profile_dir / 'run-1').mkdir(parents=True)
    sampler_log = (profile_dir / 'sampler.log').open('w')
command = [str(root / 'build/pcemu'), '--headless', '--machine', 'win95',
           '--disk', str(disk), '--ram-mb', '64', '--execute-vgabios', '--steps', '0',
           '--control-file', str(control), '--frame', str(frame), '--state-json', str(state)+'.json']
if args.jit_traces:
    command.append('--jit-traces')
elif args.jit_follow_jumps:
    command.append('--jit-follow-jumps')
with (work / 'guest.log').open('w') as log:
    guest = subprocess.Popen(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
    metadata = {'pid': guest.pid, 'command': command, 'sourceDisk': str(source),
                'scope': 'cold boot and guest restart' if args.restart else 'cold boot only',
                'emulatorSha256': emulator_hash,
                'profiled': args.sample,
                'gitCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root,
                                                      text=True).strip()}
    (work / 'run.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(f'Win95 guest PID {guest.pid}; evidence: {work}', flush=True)
    try:
        if args.sample:
            # -mayDie ends collection when this owned guest exits, retaining
            # symbols even if shutdown precedes the requested duration.
            duration = int(args.timeout * (2 if args.restart else 1) + 60)
            sampler = subprocess.Popen(['sample', str(guest.pid), str(duration), '1', '-mayDie',
                                        '-file', str(profile_dir / 'run-1/sample.txt')],
                                       stdout=sampler_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 10
            while 'Sampling process' not in (profile_dir / 'sampler.log').read_text():
                if sampler.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Host sampler did not attach; see host-profile/sampler.log')
                time.sleep(.005)
            # Symbol discovery may consume the initial pause. A second named
            # barrier is queued only after the sampler confirms attachment.
            with control.open('a') as output:
                output.write('profile-wait 3000\n')
        subprocess.run(['python3', str(root / 'tools/check_win95_restart.py'), str(control),
                        str(frame), str(state), str(disk), '--pid', str(guest.pid),
                        '--output', str(work / 'verification'), '--timeout', str(args.timeout),
                        '--restart-only' if args.restart else '--desktop-only'],
                       cwd=root, check=True)
        if sampler and sampler.poll() is not None:
            raise RuntimeError('Host sampler ended before desktop verification completed')
        with control.open('a') as output:
            output.write('quit\n')
        guest.wait(timeout=10)
        if guest.returncode:
            raise RuntimeError(f'Guest exited with status {guest.returncode}')
        if hashlib.sha256((root / 'build/pcemu').read_bytes()).hexdigest() != emulator_hash:
            raise RuntimeError('Emulator binary changed during verification')
        if sampler:
            sampler.wait(timeout=30)
            sampler_log.close()
            sample_file = profile_dir / 'run-1/sample.txt'
            graph = sample_file.read_text().split('Call graph:', 1)[-1].split('Total number in stack', 1)[0]
            if 'Thread_' not in graph or 'waitForProfiler(' not in graph:
                raise RuntimeError('Host stacks did not cover the initial attachment barrier')
            subprocess.run(['python3', str(root / 'tools/analyze_samples.py'), str(profile_dir),
                            '--output', str(work / 'host-profile.json')], cwd=root, check=True)
            metadata['hostProfile'] = {
                'summary': str(work / 'host-profile.json'),
                'sample': str(sample_file),
                'sampleSha256': hashlib.sha256(sample_file.read_bytes()).hexdigest(),
                'scope': 'Host attachment after initial dispatch batch through guest exit after desktop verification; includes OCR/dialog handling and control captures; not an isolated boot-time benchmark',
                'attachmentBarrierMs': 6000,
            }
    except Exception:
        log.flush()
        status = f'owned guest PID {guest.pid} left alive' if guest.poll() is None else \
                 f'guest exited with status {guest.returncode}'
        print(f'FAIL: {status}; disk and diagnostics retained in {work}', flush=True)
        raise
    finally:
        if sampler and sampler.poll() is None:
            sampler.terminate()
            sampler.wait(timeout=5)
        if sampler_log and not sampler_log.closed:
            sampler_log.close()
# This path was created by this invocation, never supplied by the caller.
disk.unlink()
metadata['privateDiskRemoved'] = True
metadata['verification'] = json.loads((work / 'verification/result.json').read_text())
(work / 'result.json').write_text(json.dumps(metadata, indent=2)+'\n')
print(f'PASS: Win95 verified; private compressed disk removed; evidence: {work}', flush=True)
