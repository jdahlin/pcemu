#!/usr/bin/env python3
"""Repeat a framebuffer-verified guest task from an immutable snapshot.

Wall times include control/capture/poll overhead; restore is reported separately.
macOS process CPU time measures the task response interval including guest and
control/video work, excluding descheduled/blocked time. Successful runs discard
private disk deltas and temporary frames. Failure leaves its live guest intact.
"""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
import signal
import shutil
import statistics
import subprocess
import time
from pathlib import Path
from host_process_cpu import read_process_cpu, cpu_interval

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('snapshot', type=Path)
p.add_argument('--task', type=Path, required=True)
p.add_argument('--restore-timeout', type=float, default=10, help='seconds to wait for initial snapshot pixels; separate from task completion timeout')
p.add_argument('--shutdown-timeout', type=float, default=30, help='seconds to allow clean shutdown and disk flush after task timing ends')
p.add_argument('--repeat', type=int, default=5)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--no-jit', action='store_true')
p.add_argument('--emulator', type=Path, help='alternate executable for controlled comparisons')
p.add_argument('--sample', action='store_true', help='macOS sample call stacks during the task; profiled timings include sampler overhead')
p.add_argument('--instruments', action='store_true', help='record a macOS Instruments Time Profiler trace around the task')
p.add_argument('--jit-link-warmup', type=int, help='native entries before experimental link promotion')
p.add_argument('--jit-direct-links', action='store_true', help='enable experimental same-page direct native links')
p.add_argument('--jit-debug-map', action='store_true', help='save generated ARM64 bytes, guest locations and timestamped region maps (diagnostic overhead)')
p.add_argument('--guest-profile', action='store_true', help='record opcode/dispatch counters with build/pcemu-profile')
p.add_argument('--gui', action='store_true', help='include the normal SDL frontend rather than headless operation')
p.add_argument('--sample-wait-ms', type=int, default=3000, help='named guest-side attachment barrier; requires a current emulator (0 for older executables)')
jump_mode = p.add_mutually_exclusive_group()
jump_mode.add_argument('--jit-follow-jumps', dest='jit_follow_jumps', action='store_true')
jump_mode.add_argument('--jit-basic-blocks', dest='jit_follow_jumps', action='store_false')
jump_mode.add_argument('--jit-traces', dest='jit_follow_jumps', action='store_const', const='traces')
p.set_defaults(jit_follow_jumps=None)
a = p.parse_args()
if a.jit_follow_jumps and a.no_jit:
    p.error('--jit-follow-jumps requires JIT execution')
if not 0 < a.restore_timeout <= 3600:
    p.error('--restore-timeout must be positive and at most 3600 seconds')
if not 0 < a.shutdown_timeout <= 3600:
    p.error('--shutdown-timeout must be positive and at most 3600 seconds')
if not 1 <= a.repeat <= 100:
    p.error('--repeat must be 1..100')
if not 0 <= a.sample_wait_ms <= 10000:
    p.error('--sample-wait-ms must be 0..10000')
root = Path(__file__).resolve().parent.parent
engine = a.emulator.resolve() if a.emulator else root/'build'/('pcemu-profile' if a.guest_profile else 'pcemu')
if not engine.is_file():
    p.error('Missing emulator executable: '+str(engine))
engine_hash = hashlib.sha256(engine.read_bytes()).hexdigest()
if a.sample and not shutil.which('sample'):
    p.error('macOS sample tool is unavailable')
if a.instruments and (a.sample or a.guest_profile):
    p.error('--instruments cannot be combined with --sample or --guest-profile')
if a.instruments and not shutil.which('xcrun'):
    p.error('xcrun is unavailable')
host_input_suppressed = a.gui and '--no-host-input' in subprocess.run(
    [str(engine), '--help'], check=True, capture_output=True, text=True).stdout
snapshot = a.snapshot.resolve()
task_data = a.task.read_bytes()
task_hash = hashlib.sha256(task_data).hexdigest()
task = json.loads(task_data)
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
    if a.jit_direct_links:
        command.append('--jit-direct-links')
        if a.jit_link_warmup is not None:
            command += ['--jit-link-warmup', str(a.jit_link_warmup)]
    if a.jit_debug_map:
        command += ['--jit-debug-map', str((work/'jit-debug.jsonl').resolve())]
    if a.no_jit:
        command.append('--no-jit')
    if a.jit_follow_jumps == 'traces':
        command.append('--jit-traces')
    elif a.jit_follow_jumps is True:
        command.append('--jit-follow-jumps')
    elif a.jit_follow_jumps is False:
        command.append('--jit-basic-blocks')
    started = time.perf_counter()
    # Preserve a failed guest when the invoking terminal cleans up its group.
    child = subprocess.Popen(command, cwd=root, stdout=log, stderr=subprocess.STDOUT,
                             start_new_session=True)
    sampler = None
    sampler_log = None
    instruments = None
    instruments_log = None
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
        initial = observe(before, a.restore_timeout)
        restore_ms = (time.perf_counter()-started)*1000
        if host_input_suppressed and initial.get('hostInput') != 0:
            raise RuntimeError('Automated GUI did not disable physical host input')
        follow_jumps_mode = bool(initial.get('jitFollowJumps', a.jit_follow_jumps or False))
        trace_mode = bool(initial.get('jitTraces', a.jit_follow_jumps == 'traces'))
        if a.jit_follow_jumps is not None and (follow_jumps_mode != bool(a.jit_follow_jumps) or
                trace_mode != (a.jit_follow_jumps == 'traces')):
            raise RuntimeError('Guest jump joining mode differs from requested mode')
        if initial['executionEpoch'] != 1:
            raise RuntimeError('Guest did not start from the requested snapshot')
        if a.instruments:
            instruments_log = (work/'instruments.log').open('w')
            instruments = subprocess.Popen([
                'xcrun', 'xctrace', 'record', '--template', 'Time Profiler',
                '--attach', str(child.pid), '--time-limit', '45s',
                '--output', str((work/'instruments.trace').resolve()), '--no-prompt',
            ], stdout=instruments_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic()+15
            while 'Ctrl-C to stop the recording' not in (work/'instruments.log').read_text():
                if instruments.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Instruments did not attach; see instruments.log')
                time.sleep(.01)
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
            initial = observe(before, a.restore_timeout)
        if a.instruments and a.sample_wait_ms:
            send('profile-wait '+str(a.sample_wait_ms))
            initial = observe(before, a.restore_timeout)
        if a.guest_profile:
            send('profile-start')
        cpu_before = None
        try:
            cpu_before = read_process_cpu(child.pid)
            cpu_measurement = None
        except (OSError, ValueError, NotImplementedError) as error:
            cpu_measurement = {'status': 'unavailable', 'reason': str(error)}
        began = time.perf_counter()
        task_start_utc = datetime.now(timezone.utc).isoformat() if a.instruments else None
        send(*task['commands'])
        final = observe(after, task.get('timeout',10))
        task_end_utc = datetime.now(timezone.utc).isoformat() if a.instruments else None
        task_ms = (time.perf_counter()-began)*1000
        if cpu_before is not None:
            try:
                cpu_measurement = cpu_interval(cpu_before, read_process_cpu(child.pid))
            except (OSError, ValueError, NotImplementedError) as error:
                cpu_measurement = {'status': 'unavailable', 'reason': str(error)}
        sample = {'restoreReadyMs':restore_ms,'taskResponseMs':task_ms,
                  'processCpuTime':cpu_measurement,
                  'instructions':final['instructions']-initial['instructions'],
                  'nativeInstructions':final['nativeInstructions']-initial['nativeInstructions'],
                  'compiledBlocks':final['compiledBlocks']-initial['compiledBlocks']}
        if a.instruments:
            sample['taskStartUtc'] = task_start_utc
            sample['taskEndUtc'] = task_end_utc
            sample['instrumentsTrace'] = str(work/'instruments.trace')
        if 'nativeMemoryCalls' in final and 'nativeMemoryCalls' in initial:
            sample['nativeMemoryCalls'] = final['nativeMemoryCalls'] - initial['nativeMemoryCalls']
        for counter in ('directLinkTransitions', 'directLinkPromotions', 'directLinkBlocks'):
            if counter in final and counter in initial:
                sample[counter] = final[counter] - initial[counter]
        if 'executionWallNs' in final and 'executionWallNs' in initial:
            sample['executionWallMs'] = (final['executionWallNs']-initial['executionWallNs'])/1e6
        results.append(sample)
        if index == a.repeat-1:
            subprocess.run(['sips','-s','format','png',str(frame),'--out',str(a.output/'last.png')],
                           check=True,stdout=subprocess.DEVNULL)
        shutdown_started = time.perf_counter()
        if instruments:
            instruments.send_signal(signal.SIGINT)
            if instruments.wait(timeout=30):
                raise RuntimeError('Instruments recording failed; see instruments.log')
            instruments_log.close()
            if not (work/'instruments.trace').exists():
                raise RuntimeError('Instruments did not save a trace')
        if a.guest_profile:
            send('profile-stop '+str((work/'guest-profile.json').resolve()), 'quit')
        else:
            send('quit')
        child.wait(timeout=a.shutdown_timeout)
        sample['shutdownWaitMs'] = (time.perf_counter()-shutdown_started)*1000
        sample['exitStatus'] = child.returncode
        if child.returncode != 0:
            raise RuntimeError(f'Guest shutdown failed with status {child.returncode}; files retained')
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
        if instruments and instruments.poll() is None:
            instruments.terminate()
            instruments.wait(timeout=5)
        if instruments_log and not instruments_log.closed:
            instruments_log.close()
        if sampler and sampler.poll() is None:
            sampler.terminate()
            sampler.wait(timeout=5)
        log.flush()
        status = f'guest PID {child.pid} left alive' if child.poll() is None else f'guest exited with status {child.returncode}'
        print(f'Run failed; {status}; files {work} retained',flush=True)
        raise
if hashlib.sha256(engine.read_bytes()).hexdigest() != engine_hash or \
        hashlib.sha256(a.task.read_bytes()).hexdigest() != task_hash:
    raise RuntimeError('Emulator or task changed during measurement; results not accepted')
report={'task':task['name'],'snapshot':str(snapshot),'jit':not a.no_jit,'profiled':a.sample,
        'instrumentsProfiled':a.instruments,
        'jitDebugMapped':a.jit_debug_map,
        'directLinks':a.jit_direct_links,
        'followJumps':follow_jumps_mode, 'traces':trace_mode,
        'emulatorSha256':engine_hash,
        'taskSha256':task_hash, 'restoreTimeoutSeconds':a.restore_timeout,
        'shutdownTimeoutSeconds':a.shutdown_timeout,
        'guestProfiled':a.guest_profile,
        'frontend':'SDL' if a.gui else 'headless',
        'hostInputSuppressed':bool(host_input_suppressed),
        'emulator':str(engine),
        'scope':'end-to-end process/control/capture/poll latency; not isolated CPU time',
        'processCpuTimeScope':'emulator process user+system CPU during task response; includes guest/control/video and all process threads, excludes restore, blocked IO and descheduled time; not isolated guest execution time',
        'samples':results,'medianRestoreReadyMs':statistics.median(r['restoreReadyMs'] for r in results),
        'medianTaskResponseMs':statistics.median(r['taskResponseMs'] for r in results)}
cpu_samples = [r['processCpuTime']['totalMs'] for r in results
               if r['processCpuTime']['status'] == 'available']
report['medianProcessCpuMs'] = statistics.median(cpu_samples) if len(cpu_samples) == len(results) else None
(a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS: {a.repeat} independent restores produced the expected pixels',flush=True)
