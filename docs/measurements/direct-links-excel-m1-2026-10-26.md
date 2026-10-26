# Direct-link milestone: Excel 97 dependent recalculation on M1 Pro

The implementation at `202d2edcd4e6b0fc45004db272d20ef013492670` improves complete
application task CPU time while passing the invalidation, fault and interrupt
suites. The measured win is Excel recalculation; Word startup still regresses.
Direct linking remains opt-in.

## Workload and readiness

Use the pre-existing recovered Excel fixture, without reinstalling Office or
creating a raw disk copy. The workbook has `B1:B4096 =ROW()*$D$1+7` and
`C1 =SUM(B1:B4096)`. Editing D1 from 3 to 4 changes the displayed SUM from
25200640 to 33591296. Timing begins at the text/Enter commands and ends at exact
reference pixels for the final SUM. This covers the complete dependent
recalculation, including emulator control, capture, video, device and interrupt
work. It excludes checkpoint restore and shutdown. Process user+system CPU
excludes blocked/descheduled time. All runs are headless and unprofiled.

The task file is
[excel-dependent-recalc-2026-10-25.task.json](excel-dependent-recalc-2026-10-25.task.json),
SHA-256 `e2dad7207c436cf470f93a11d413eb59e6d50142a15c654d61710dea2cba888a`.
Every linked run executes approximately 1.403 billion guest instructions and
166 million real direct transitions. The target result comes from the full
4,096-row dependency chain, rather than an idle or partial UI state.

The local checkpoint is `build/snapshots/excel-recalc-local`. Its 2.3 MiB QCOW2
child backs onto the existing local flattened Word disk. Both source legacy
children share the same Office parent. Their union contains 114 modified 4 KiB
blocks; replacing those with source Excel bytes and verifying full readback
makes the child logically equal to the recovered Excel disk. Outside the union,
both inherit the same parent. `qemu-img check` passes. State, preview and task
files are copied unchanged. Source hashes and the derivation proof are in the
JSON report. No large new fixture or raw staging image was created.

## Five rotating rounds

Each round runs original baseline, current linked, and current off sequentially;
the starting mode rotates each round. There are no overlapping benchmarks,
concurrent builds/profilers, outlier exclusions or failed runs. All fifteen
independent restores produce the expected pixels and exit with status zero.

| Build/mode | Median process CPU | Median task response |
| --- | ---: | ---: |
| Original baseline, 57ce863 | 7341.6 ms | 7361.8 ms |
| Current, links off | 7659.5 ms | 7671.9 ms |
| Current, links on, warmup 256 | 6400.1 ms | 6428.8 ms |

Linked CPU is 12.8% below the original baseline median and 16.4% below the current
off median. Same-round reductions versus original baseline are 12.8%, 11.6%,
14.7%, 12.6% and 12.9%. The rotating comparison and consistent per-round direction
support the improvement despite variation in host conditions. These are results
from this M1 Pro and this workload, not a claim of general performance gains.

## Frozen build and correctness

A clean `git archive 202d2ed` was built as Release in
`build/direct-links-final`. Its executable SHA-256 is
`07e80d7ca9ba78c23fa3f3f9ebf0002484832689406d40ad866e033e7f489823`, identical
to the earlier candidate used in the first three rounds. The last two rounds
use the fresh frozen executable. The original baseline executable SHA-256 is
`96108846b758cb65e6a42e3c49b35bd0588ef02c0e691ca0b81547050710b7c3`.
The report includes production source hashes, individual samples and CTest log.

The frozen Release build passes `tests`, `jit-batch-tests` and
`jit-cached-batch-tests`. Coverage includes interpreter comparisons, self-modifying
code, PTE remapping without physical code-generation changes, exact partial page
fault state, MMIO replay counts, RAM/generation/context replacement, repeated
incoming-source replacement and native arena reuse, ADC carry spills, delayed
promotion without retranslation, and PIT deadlines with links enabled. Timer
tests reject early interrupts and latency above the existing 127-instruction
bound. The cached-batch suite passes 151,150 checks. Four debug-map and two
Instruments-analysis Python tests also pass.

## Reproduction

Build the archived implementation with CMake Release and use the immutable local
checkpoint and committed task. For example:

```sh
python3 tools/bench_snapshot_task.py build/snapshots/excel-recalc-local \
  --task docs/measurements/excel-dependent-recalc-2026-10-25.task.json \
  --emulator build/direct-links-final/pcemu --jit-direct-links \
  --jit-link-warmup 256 --repeat 1 --restore-timeout 30 \
  --output build/excel-links-recheck
```

For a controlled comparison, execute five sequential rounds rotating these
three modes: original baseline executable without link flags, frozen current
executable with the flags above, and frozen current executable without link
flags. Give every invocation a fresh output path and keep all results. Run
correctness checks separately from performance measurements:

```sh
ctest --test-dir build/direct-links-final \
  -R '^(tests|jit-batch-tests|jit-cached-batch-tests)$' --output-on-failure
```

## Limit and decision

The first direct-link application CPU gate passes for Excel recalculation.
Word document startup remains 3.2–3.8% slower in its separate rotating comparison;
see [the Word results](word5m-m1-2026-10-26.md) and
[ownership experiment](direct-links-unique-word5m-2026-10-26.json).
Keep linking off by default. Longer repeated native chains justify further
work, while cold Word startup needs separate attention to interpreter and
compilation costs. Neither the Word regression nor failed selectivity experiments
are excluded from the evaluation.
