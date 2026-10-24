#!/usr/bin/env python3
"""Repeat a framebuffer-verified guest task from an immutable snapshot.

Times include process startup/control/capture/poll overhead; they are end-to-end
upper bounds, not isolated CPU execution time. Successful runs discard their
private disk deltas and temporary frames. Failure leaves its live guest intact.
"""
import argparse
import base64
import json
import shutil
import statistics
import subprocess
import time
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('snapshot', type=Path)
p.add_argument('--task', type=Path, required=True)
p.add_argument('--repeat', type=int, default=5)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--no-jit', action='store_true')
p.add_argument('--emulator', type=Path, help='alternate executable for controlled comparisons')
p.add_argument('--sample', action='store_true', help='macOS sample call stacks during the task; profiled timings include sampler overhead')
p.add_argument('--guest-profile', action='store_true', help='record opcode/dispatch counters with build/pcemu-profile')
p.add_argument('--gui', action='store_true', help='include the normal SDL frontend rather than headless operation')
p.add_argument('--sample-wait-ms', type=int, default=3000, help='named guest-side attachment barrier; requires a current emulator (0 for older executables)')
a = p.parse_args()
if not 1 <= a.repeat <= 100:
    p.error('--repeat must be 1..100')
if not 0 <= a.sample_wait_ms <= 10000:
    p.error('--sample-wait-ms must be 0..10000')
root = Path(__file__).resolve().parent.parent
engine = a.emulator.resolve() if a.emulator else root/'build'/('pcemu-profile' if a.guest_profile else 'pcemu')
if not engine.is_file():
    p.error('Missing emulator executable: '+str(engine))
if a.sample and not shutil.which('sample'):
    p.error('macOS sample tool is unavailable')
host_input_suppressed = a.gui and '--no-host-input' in subprocess.run(
    [str(engine), '--help'], check=True, capture_output=True, text=True).stdout
snapshot = a.snapshot.resolve()
task = json.loads(a.task.read_text())
rect = task['rectangle']
before = base64.b64decode(task['before'], validate=True)
after = base64.b64decode(task['after'], validate=True)
x0, y0, x1, y1 = rect
if not 0 <= x0 < x1 <= 1024 or not 0 <= y0 < y1 <= 1024:
    p.error('Invalid task rectangle')
if before == after or len(before) != (x1-x0)*(y1-y0)*3 or len(after) != len(before):
    p.error('Task must distinguish before and after pixels')
a.output.mkdir(parents=True, exist_ok=True)
results = []

def patch(frame):
    header = frame.read_bytes().split(b'\n', 3)
    width, height = map(int, header[1].split())
    data = header[3]
    if header[0] != b'P6' or len(data) != width*height*3 or x1 > width or y1 > height:
        raise ValueError('Incomplete or unexpected framebuffer')
    return b''.join(data[(y*width+x0)*3:(y*width+x1)*3] for y in range(y0, y1))

for index in range(a.repeat):
    work = a.output / f'run-{index}'
    work.mkdir(exist_ok=False)
    control, frame, metadata = (work/name for name in ('control.txt','frame.ppm','state.json'))
    control.touch()
    log = (work/'guest.log').open('w')
    command = [str(engine), '--steps', '0',
               '--load-snapshot', str(snapshot), '--control-file', str(control.resolve()),
               '--frame', str(frame.resolve()), '--state-json', str(metadata.resolve())]
    if not a.gui:
        command.append('--headless')
    elif host_input_suppressed:
        command.append('--no-host-input')
    if a.no_jit:
        command.append('--no-jit')
    started = time.perf_counter()
    child = subprocess.Popen(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
    sampler = None
    sampler_log = None
    def send(*commands):
        with control.open('a') as f:
            f.write(''.join(command+'\n' for command in commands))
    def observe(expected, timeout=10):
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            if child.poll() is not None:
                raise RuntimeError(f'Guest exited unexpectedly: {child.returncode}')
            old = frame.stat().st_mtime_ns if frame.exists() else 0
            send('capture')
            while time.monotonic() < deadline:
                try:
                    if frame.stat().st_mtime_ns > old and patch(frame) == expected:
                        return json.loads(metadata.read_text())
                    if frame.stat().st_mtime_ns > old:
                        break
                except (FileNotFoundError, ValueError, IndexError, json.JSONDecodeError):
                    pass
                time.sleep(.005)
            time.sleep(.005)
        raise RuntimeError('Expected framebuffer state not observed before timeout')
    try:
        initial = observe(before)
        restore_ms = (time.perf_counter()-started)*1000
        if host_input_suppressed and initial.get('hostInput') != 0:
            raise RuntimeError('Automated GUI did not disable physical host input')
        if initial['executionEpoch'] != 1:
            raise RuntimeError('Guest did not start from the requested snapshot')
        if a.sample:
            sampler_log = (work/'sampler.log').open('w')
            sampler = subprocess.Popen(['sample', str(child.pid), '120', '1', '-mayDie',
                                        '-file', str((work/'sample.txt').resolve())],
                                       stdout=sampler_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic()+10
            while 'Sampling process' not in (work/'sampler.log').read_text():
                if sampler.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Host sampler did not attach; see sampler.log')
                time.sleep(.005)
            # Counter baseline excludes sampler attachment and idle guest work.
            if a.sample_wait_ms:
                send('profile-wait '+str(a.sample_wait_ms))
            initial = observe(before)
        if a.guest_profile:
            send('profile-start')
        began = time.perf_counter()
        send(*task['commands'])
        final = observe(after, task.get('timeout',10))
        task_ms = (time.perf_counter()-began)*1000
        sample = {'restoreReadyMs':restore_ms,'taskResponseMs':task_ms,
                  'instructions':final['instructions']-initial['instructions'],
                  'nativeInstructions':final['nativeInstructions']-initial['nativeInstructions'],
                  'compiledBlocks':final['compiledBlocks']-initial['compiledBlocks']}
        if 'executionWallNs' in final and 'executionWallNs' in initial:
            sample['executionWallMs'] = (final['executionWallNs']-initial['executionWallNs'])/1e6
        results.append(sample)
        if index == a.repeat-1:
            subprocess.run(['sips','-s','format','png',str(frame),'--out',str(a.output/'last.png')],
                           check=True,stdout=subprocess.DEVNULL)
        if a.guest_profile:
            send('profile-stop '+str((work/'guest-profile.json').resolve()), 'quit')
        else:
            send('quit')
        child.wait(timeout=5)
        if sampler:
            # -mayDie retains symbols and ends collection when the guest exits.
            # This bounds sampling to the task instead of appending idle time.
            sampler.wait(timeout=30)
            sampler_log.close()
            if not (work/'sample.txt').exists():
                raise RuntimeError('Sampler did not produce a call graph')
            graph = (work/'sample.txt').read_text().split('Call graph:', 1)[-1].split('Total number in stack', 1)[0]
            if 'Thread_' not in graph:
                raise RuntimeError('No stacks collected before guest exit; increase the attachment barrier')
            if a.sample_wait_ms and 'waitForProfiler(' not in graph:
                raise RuntimeError('Profiler attachment barrier was not observed in the stacks')
        log.close()
        disk = Path(final['disk']) if final['disk'] else None
        expected_root = (snapshot.parent/'.runs').resolve()
        if disk and disk.parent.parent.resolve() == expected_root and \
                disk.parent.name.startswith(f'{snapshot.name}-{child.pid}-'):
            shutil.rmtree(disk.parent)
        frame.unlink(missing_ok=True)
        print(f'Run {index+1}: ready {restore_ms:.1f} ms; task {task_ms:.1f} ms',flush=True)
    except Exception:
        if sampler and sampler.poll() is None:
            sampler.terminate()
            sampler.wait(timeout=5)
        log.flush()
        status = f'guest PID {child.pid} left alive' if child.poll() is None else f'guest exited with status {child.returncode}'
        print(f'Run failed; {status}; files {work} retained',flush=True)
        raise
report={'task':task['name'],'snapshot':str(snapshot),'jit':not a.no_jit,'profiled':a.sample,
        'guestProfiled':a.guest_profile,
        'frontend':'SDL' if a.gui else 'headless',
        'hostInputSuppressed':bool(host_input_suppressed),
        'emulator':str(engine),
        'scope':'end-to-end process/control/capture/poll latency; not isolated CPU time',
        'samples':results,'medianRestoreReadyMs':statistics.median(r['restoreReadyMs'] for r in results),
        'medianTaskResponseMs':statistics.median(r['taskResponseMs'] for r in results)}
(a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS: {a.repeat} independent restores produced the expected pixels',flush=True)
