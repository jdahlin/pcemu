#!/usr/bin/env python3
"""Freeze optimized sources/binaries and collect separate Quake baseline/host/guest profiles."""
import argparse
import collections
import hashlib
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RELEASE_FLAGS = '-O3 -DNDEBUG -g -gdwarf-4 -fno-omit-frame-pointer'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize_baselines(runs):
    """Use exact frame-window timings only; never mix diagnostic FPS into scores."""
    if len(runs) < 3:
        raise ValueError('At least three independent baseline runs are required')
    identities = {(r['executableSha256'], r['demoSha256'], r['frames']) for r in runs}
    if len(identities) != 1 or any(not r['scoreEligible'] or r['profileMode'] != 'none' for r in runs):
        raise ValueError('Baseline binaries, workloads or profiling modes differ')
    for r in runs:
        w = r['window']
        if any(not math.isfinite(w[k]) or w[k] <= 0 for k in ['wallSeconds', 'threadCpuSeconds']):
            raise ValueError('Invalid baseline clock interval')
        if w.get('guestProfiled') or w.get('hostSampled'):
            raise ValueError('Profiled frame windows cannot be baseline scores')
    values = [r['windowFps'] for r in runs]
    if any(not math.isfinite(x) or x <= 0 for x in values):
        raise ValueError('Invalid baseline timing')
    median = statistics.median(values)
    cpu = [r['frames'] / r['window']['threadCpuSeconds'] for r in runs]
    return {'runs': len(runs), 'medianFps': median, 'medianThreadCpuFps': statistics.median(cpu),
            'minFps': min(values), 'maxFps': max(values),
            'madFps': statistics.median(abs(v - median) for v in values),
            'cvPercent': 100 * statistics.stdev(values) / statistics.mean(values),
            'valuesFps': values,
            'threadCpuToWallRatios': [r['window']['threadCpuSeconds'] / r['window']['wallSeconds'] for r in runs],
            'firstToLastChangePercent': 100 * (values[-1] / values[0] - 1)}


def analyze_sample(text):
    """Derive main-thread self counts from parent minus immediate child counts.

    Inclusive rows overlap. Recursive appearances are deduplicated along a path;
    do not mistake leaf-only percentages for whole-function time fractions.
    """
    if 'Call graph:' not in text:
        raise ValueError('Sample has no call graph')
    graph = text.split('Call graph:', 1)[1].split('Total number in stack', 1)[0]
    nodes, stack = [], []
    selected = False
    for line in graph.splitlines():
        match = re.match(r'^([\s+|!:]*)(\d+)\s+(.+)$', line)
        if not match:
            continue
        prefix, count, label = match.groups()
        if label.startswith('Thread_'):
            if selected: break
            selected = 'com.apple.main-thread' in label
            if not selected: continue
        if not selected: continue
        depth, count = len(prefix), int(count)
        name = re.split(r'\s+\(in ', label, maxsplit=1)[0].strip()
        while stack and nodes[stack[-1]]['depth'] >= depth:
            stack.pop()
        node = {'depth': depth, 'count': count, 'children': 0, 'name': name,
                'recursive': any(nodes[i]['name'] == name for i in stack)}
        if stack: nodes[stack[-1]]['children'] += count
        nodes.append(node); stack.append(len(nodes) - 1)
    if not nodes:
        raise ValueError('Sample has no identifiable main thread')
    self_counts, inclusive = collections.Counter(), collections.Counter()
    for node in nodes[1:]:
        if node['children'] > node['count']:
            raise ValueError('Inconsistent sample tree; refusing a misleading ranking')
        self_counts[node['name']] += node['count'] - node['children']
        if not node['recursive']: inclusive[node['name']] += node['count']
    if sum(self_counts.values()) != nodes[0]['count']:
        raise ValueError('Sample self counts do not conserve the main-thread total')
    idle_terms = ('nanosleep', '__semwait_signal', 'usleep', '__psynch_cvwait', 'mach_msg')
    idle = sum(n for name, n in self_counts.items() if any(term in name for term in idle_terms))
    total = nodes[0]['count']; active = total - idle
    def rows(counter):
        return [{'symbol': name, 'samples': n, 'percentOfMainThread': 100 * n / total}
                for name, n in counter.most_common() if n]
    return {'mainThreadSamples': total, 'idleLeafSamples': idle, 'nonIdleSamples': active,
            'unresolvedLeafSamples': sum(n for name, n in self_counts.items() if '???' in name),
            'self': rows(self_counts), 'inclusive': rows(inclusive),
            'notes': ['Host sampling is diagnostic; its FPS is excluded from baseline statistics.',
                      'Raw sampling includes the attachment barrier and short exit tail; idle leaves are reported separately.',
                      'Inclusive percentages overlap and cannot be added. Self counts conserve the main-thread total.']}


def freeze_sources(destination):
    """Copy nonignored build inputs; include working changes and record each hash."""
    paths = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT).split(b'\0')
    files = []
    for raw in sorted(set(paths)):
        if not raw: continue
        name = os.fsdecode(raw)
        if name != 'CMakeLists.txt' and name != 'Makefile' and not name.startswith(('src/', 'cmake/', 'firmware/', 'tools/', 'tests/', 'platform/', 'guest/', 'roms/')):
            continue
        source = ROOT / name
        if not source.is_file(): continue
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        files.append({'path': name, 'sha256': sha(target)})
    if any(sha(ROOT / row['path']) != row['sha256'] for row in files):
        raise RuntimeError('Sources changed during capture; rerun to obtain a coherent frozen build')
    return files


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--disk', type=Path, default=ROOT / 'media/quake.qcow2')
    p.add_argument('--frames', type=int, default=50)
    p.add_argument('--repeat', type=int, default=5)
    p.add_argument('--timeout', type=int, default=180)
    p.add_argument('--output', type=Path)
    p.add_argument('--no-host-sample', action='store_true', help='collect counters without macOS native stacks')
    a = p.parse_args()
    if a.repeat < 3 or a.frames < 1 or a.timeout < 1 or not a.disk.is_file():
        p.error('Need an existing Quake disk, positive frames/timeout, and at least three repeats')
    if not a.no_host_sample and (sys.platform != 'darwin' or not shutil.which('sample')):
        p.error('macOS sample is required; use --no-host-sample for counters only')
    work = a.output.resolve() if a.output else Path(tempfile.mkdtemp(prefix='quake-profile-', dir=ROOT / 'build'))
    if a.output: work.mkdir(parents=True, exist_ok=False)
    source = work / 'source'; source.mkdir()
    inputs = freeze_sources(source)
    metadata = {'schemaVersion': 1, 'gitHead': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'gitStatus': subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True),
        'sourceFiles': inputs, 'sourceManifestSha256': hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest(),
        'host': platform.platform(), 'architecture': platform.machine(), 'logicalCpus': os.cpu_count(),
        'loadAtStart': os.getloadavg(), 'buildType': 'Release', 'releaseFlags': RELEASE_FLAGS,
        'frames': a.frames, 'repeat': a.repeat, 'disk': str(a.disk.resolve())}
    (work / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    build = source / 'build'
    print(f'Frozen optimized build and profiles: {work}', flush=True)
    with (work / 'build.log').open('w') as log:
        subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_CXX_FLAGS_RELEASE=' + RELEASE_FLAGS], stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(['cmake', '--build', str(build), '--target', 'quake-bench', 'quake-bench-profile', 'disk-tool'],
            stdout=log, stderr=subprocess.STDOUT, check=True)
    normal, diagnostic = build / 'quake-bench', build / 'quake-bench-profile'
    cache = (build / 'CMakeCache.txt').read_text()
    compiler = re.search(r'^CMAKE_CXX_COMPILER:FILEPATH=(.+)$', cache, re.MULTILINE).group(1)
    metadata['compilerVersion'] = subprocess.check_output([compiler, '--version'], text=True)
    metadata['compileCommandsSha256'] = sha(build / 'compile_commands.json')
    chain = json.loads(subprocess.check_output(['qemu-img', 'info', '--backing-chain', '--output=json', str(a.disk.resolve())]))
    metadata['guestDiskChain'] = [{'path': str(Path(row['filename']).resolve()), 'sha256': sha(Path(row['filename']))} for row in chain]
    if sys.platform == 'darwin' and not a.no_host_sample:
        with (work / 'dsymutil.log').open('w') as log:
            subprocess.run(['dsymutil', str(normal), '-o', str(normal) + '.dSYM'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        metadata['debugSymbols'] = {'format': 'DWARF4', 'bundle': str(normal) + '.dSYM'}
    metadata['binaries'] = {name: sha(build / name) for name in ['quake-bench', 'quake-bench-profile', 'disk-tool']}
    (work / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    runs = []
    def run(name, mode='none'):
        output = work / name
        engine = diagnostic if mode == 'counters' else normal
        command = [sys.executable, str(source / 'tools/bench_quake.py'), '--disk', str(a.disk.resolve()),
                   '--frames', str(a.frames), '--timeout', str(a.timeout), '--runner', str(engine), '--no-build',
                   '--backend', 'interpreter', '--measure-window', '--profile-mode', mode, '--output-dir', str(output)]
        if any(sha(Path(row['path'])) != row['sha256'] for row in metadata['guestDiskChain']):
            raise RuntimeError('Guest backing chain changed; refusing mixed-workload data')
        print(f'Collecting {name} ({mode})', flush=True)
        with (work / (name + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        data = json.loads((output / 'result.json').read_text())
        if data['executableSha256'] != sha(engine) or data['frames'] != a.frames:
            raise RuntimeError('Executed binary or completed workload differs from the frozen request')
        data['artifactDirectory'] = str(output)
        runs.append({'name': name, **data})
        return data
    run('warmup') # Host caches/temperature warmup, stored but excluded from statistics.
    run('baseline-1')
    if not a.no_host_sample: run('host-sample', 'host')
    run('guest-counters', 'counters')
    for index in range(2, a.repeat + 1): run(f'baseline-{index}')
    baseline = [r for r in runs if r['name'].startswith('baseline-')]
    summary = summarize_baselines(baseline)
    warnings = []
    if summary['cvPercent'] > 5: warnings.append('Baseline variance exceeds 5%; collect more runs on an idle host.')
    if any(x < .95 for x in summary['threadCpuToWallRatios']): warnings.append('Baseline thread CPU/wall ratio below 95%; host contention or waits affected timings.')
    host = None
    if a.no_host_sample: warnings.append('Native host sampling was skipped; guest counters alone cannot establish whole-process bottlenecks.')
    if not a.no_host_sample:
        sampler_log = (work / 'host-sample/sampler.log').read_text()
        symbol_warnings = [line for line in sampler_log.splitlines() if 'DWARF' in line or 'warning' in line.lower()]
        if symbol_warnings: warnings.append('Native symbol decoder reported warnings; inspect host-sample/sampler.log.')
        host = analyze_sample((work / 'host-sample/sample.txt').read_text())
        if host['nonIdleSamples'] < 1000: warnings.append('Fewer than 1000 non-idle main-thread samples; extend the demo for stronger attribution.')
        host['symbolWarnings'] = symbol_warnings
        (work / 'host-analysis.json').write_text(json.dumps(host, indent=2) + '\n')
    sys.path.insert(0, str(source / 'tools'))
    from analyze_guest_profiles import analyze
    guest_file = work / 'guest-counters/guest-profile.json'
    guest = analyze([guest_file])
    diagnostic_window = next(r['window'] for r in runs if r['name'] == 'guest-counters')
    raw = json.loads(guest_file.read_text())
    if (raw['startInstructions'], raw['stopInstructions']) != (diagnostic_window['beginInstructions'], diagnostic_window['endInstructions']):
        raise RuntimeError('Guest counter interval does not match the validated frame window')
    x87 = sorted(raw.get('x87Instructions', []), key=lambda x: -x['attempts'])
    guest['x87Instructions'] = x87
    guest['sampledIPsByCount'] = sorted(raw['sampledIPs'], key=lambda x: -x['samples'])[:50]
    (work / 'guest-analysis.json').write_text(json.dumps(guest, indent=2) + '\n')
    report = {'schemaVersion': 1,
              'workload': {'demoSha256': baseline[0]['demoSha256'], 'frames': a.frames,
                           'resolution': [320, 200], 'sound': False, 'backend': 'interpreter',
                           'pitClock': baseline[0]['pitClock'], 'releaseFlags': RELEASE_FLAGS},
              'baselineExecutableSha256': metadata['binaries']['quake-bench'], 'scope': 'Quake 1.06 original demo1 prefix, exact timed-frame window, silent interpreter',
              'baseline': summary, 'warnings': warnings, 'runs': runs, 'loadAtEnd': os.getloadavg(),
              'hostAnalysis': 'host-analysis.json' if host else None, 'guestAnalysis': 'guest-analysis.json',
              'tenfoldTargetFps': summary['medianFps'] * 10,
              'notes': ['Only baseline-* runs contribute speed measurements; warmup/host/counter runs do not.',
                        'Optimized source and binaries are frozen; working-tree edits cannot alter this session.',
                        'Baseline timing includes the lightweight guest-IP boundary checks; sampled/counter overhead is kept separate.',
                        'Guest sampling/counter timings include instrumentation overhead and only rank diagnostic work.',
                        '50 frames cover the opening scene; validate optimizations with the full demo and correctness suite.',
                        '10x is a target, not a predicted or achieved speedup.']}
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    lines = [f'Quake profiling: {a.frames} timed frames', '',
             f'Baseline median: {summary["medianFps"]:.3f} FPS; MAD {summary["madFps"]:.3f}; CV {summary["cvPercent"]:.2f}%',
             f'Range: {summary["minFps"]:.3f}–{summary["maxFps"]:.3f} FPS',
             f'10x target: {summary["medianFps"] * 10:.3f} FPS (not achieved)', '', 'Native main-thread self samples:']
    if host:
        lines += [f'{r["percentOfMainThread"]:5.1f}% {r["symbol"]}' for r in host['self'][:12]]
    lines += ['', 'Guest opcodes by execution frequency (not CPU share):']
    lines += [f'{r["percentOfInterpretedAttempts"]:5.1f}% {r["opcode"]} {r["name"]}' for r in guest['opcodes'][:12]]
    lines += ['', 'Warnings:'] + (warnings or ['None from the automated checks.'])
    (work / 'summary.txt').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines), flush=True)
    print(f'Full reproducible evidence: {work / "report.json"}')


if __name__ == '__main__':
    main()
