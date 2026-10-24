#!/usr/bin/env python3
"""Alternate unprofiled snapshot-task runs between two emulator executables."""
import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--candidate', type=Path, default=Path('build/pcemu'))
p.add_argument('--repeat', type=int, default=5)
p.add_argument('--gui', action='store_true')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
if not 1 <= a.repeat <= 100:
    p.error('repeat must be 1..100')
a.output.mkdir(parents=True, exist_ok=False)
results = {}
for app in ('word', 'excel'):
    samples = {'baseline': [], 'candidate': []}
    point = Path(f'build/snapshots/office97-{app}-startup')
    for pair in range(a.repeat):
        order = ['baseline', 'candidate'] if pair % 2 == 0 else ['candidate', 'baseline']
        for variant in order:
            work = a.output / f'{app}-{pair}-{variant}'
            command = [sys.executable, 'tools/bench_snapshot_task.py', str(point),
                       '--task', str(point/'task.json'), '--repeat', '1', '--output', str(work),
                       '--emulator', str(getattr(a, variant))]
            if a.gui:
                command.append('--gui')
            subprocess.run(command, check=True)
            samples[variant].append(json.loads((work/'result.json').read_text())['samples'][0])
    summary = {variant: {'medianTaskResponseMs': statistics.median(s['taskResponseMs'] for s in rows),
                         'medianExecutionWallMs': statistics.median(s['executionWallMs'] for s in rows),
                         'samples': rows} for variant, rows in samples.items()}
    summary['improvementPercent'] = 100*(1-summary['candidate']['medianTaskResponseMs']/
                                          summary['baseline']['medianTaskResponseMs'])
    results[app] = summary
    (a.output/'result.json').write_text(json.dumps(results, indent=2)+'\n')
    print(f"{app}: {summary['baseline']['medianTaskResponseMs']:.1f} -> "
          f"{summary['candidate']['medianTaskResponseMs']:.1f} ms "
          f"({summary['improvementPercent']:.1f}% improvement)", flush=True)
