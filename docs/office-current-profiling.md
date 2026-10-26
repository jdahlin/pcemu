# Current Office profiling: host stacks and guest execution

Measured on the Apple M4 Mac across 25–26 October 2026, using the frozen x86
candidate containing native memory-indirect CALL (`1c5a879`). The uninstrumented
executable and a matched-source `PCEMU_PROFILE` build are identified by hashes
and complete commands in the [measurement record](measurements/office-host-profile-2026-10-26.json).
No performance optimization was introduced during this collection.

## Workloads and boundaries

- **Loaded Excel 97 calculation:** change D1 from 3 to 4 in a sheet with 4,096
  dependent rows; verify the rendered sum changes from 25,200,640 to 33,591,296.
  This measures useful work after application load, not boot or launch.
- **Word 97 warm launch:** from a prepared Run dialog containing `winword` to the
  first rendered Page 1 / Sec 1 status. Word was launched and closed when making
  this checkpoint, so OS caches/preferences are warm; JIT code is not restored.
  This is a launch profile, not a loaded document-editing benchmark.

Each workload has three independent restored runs with macOS `sample` at 1 ms
on the normal executable, plus a separate diagnostic run with guest counters.
The expected initial/final pixel patches pass in every run. Restore and named
profiler attachment waits are separate from the task; attachment-stack samples
are excluded from the percentages. Screenshot conversion and process shutdown
leave small end margins, proportionally more important for Word's short task.
Instrumentation timings are not compared against normal timings.

## Main-thread wall-stack shares

These exclusive categories do not double-count caller and callee samples.
They include blocked I/O and observation work, and are **not exact per-function
CPU durations**. Worker thread stacks are reported separately. Actual process
user+system CPU is independently recorded over the response interval with the
validated Mach timebase conversion.

| Work | Excel calculation | Word warm launch |
| --- | ---: | ---: |
| JIT dispatch/cache/validation | 27.1% | 22.9% |
| Generated ARM64 and native helpers | 23.4% | 8.7% |
| Device polling/interrupt delivery | 16.9% | 11.2% |
| Interpreter, MMU and guest faults | 9.8% | 28.9% |
| Capture/video/control output | 11.0% | 7.8% |
| JIT compilation | 0.8% | 9.8% |
| Disk/compression | 0.7% | 0.8% |
| Other host work | 8.1% | 7.1% |
| Unresolved code without JIT ancestry | 2.1% | 2.7% |

Excel contains 30,726 task samples and 26,003 excluded attachment samples;
Word contains 2,470 task samples and 25,312 excluded attachment samples. Excel's
per-run dispatch share is 26.2–27.8%; polling is 13.2–20.2%; generated code/helpers
22.1–24.4%; capture/control 8.8–13.8%. The per-run reports retain this spread.

Profiler-inclusive process CPU is 9.85/10.09/13.07 s for Excel, with response
10.57/10.97/14.96 s. Word process CPU is 0.870/0.867/0.835 s, with response
0.930/0.915/0.894 s. Shared-host scheduling, capture and profiler overhead remain;
these values describe collection scope, not a new speedup or stable benchmark.

## Concrete host hotspots

In Excel, the PS/2 due-check source location at `machine.cpp:108` accounts for
8.1% of self samples and the timer-poll location at line 106 for 2.4%. Another
1.9% maps to the function's closing line 124. These optimized source locations
include inlined work and compiler scheduling; the closing-line samples cannot
be assigned specifically to interrupt delivery, and the timer figure is not
isolated PIT multiplication time. Polling still occurs about once per
JIT/interpreter dispatch.

JIT entry validation at `jit.cpp:89` accounts for 4.8% of self samples, with
other metadata/identity guards appearing at lines 87, 99 and 107–109. The
source-site report preserves these separately from samples in generated code.
`CPU::address` has 2.0% self samples across its callers; that overlaps the
category totals and must not be added as an independent percentage.

Excel's frame/control path contains 6.1% self samples in `__write_nocancel`
and 1.4% in `__close_nocancel`. Capture is therefore still material even after
faster VGA rendering. Normal GUI presentation does not write a PPM file for
every observation, so this headless capture overhead is not all application
execution or normal display cost.

## Guest counters explain the remaining interpreter work

The separate Excel run executes 1,404,177,914 guest instructions and records
22,527,234 interpreter attempts. There are 362,340,000 dispatches, 339,923,476
native block calls and 20,630,767 negative-cache hits. That is about 3.87 guest
instructions per dispatch; 5.7% of dispatches encounter a cached interpreted
fragment. Thus high native instruction coverage can coexist with substantial
entry/poll overhead.

Remaining `FF /2` CALL attempts are only 65,268; `FF /6` memory/register PUSH
accounts for 1,332,398 attempts. MOV `8B` accounts for 1,892,803 interpreter
attempts (8.4% of interpreter attempts), followed by `FF`, immediate ALU `83`,
`0F`, `01`, CLI, and immediate-memory MOV. Supported opcodes can remain
interpreted in cold or deliberately small fragments. Attempt percentages are
not CPU percentages, and primary `0F` counts overlap extended-opcode counts.

Word records 32,106,892 guest instructions, 8,009,877 interpreter attempts,
14,980,000 dispatches and 7,030,482 native calls. Negative-cache hits are 41.6%
of dispatches. MOV `8B`, extended operations, `FF`, byte shifts and string
operations feature in the remaining attempts. Cold/short fragments and compiling
new code matter much more here than in loaded Excel. These two workloads do not
support choosing one global JIT threshold solely by analogy with JS.

## Priorities supported by this evidence

1. **Amortize dispatch and device polling.** Investigate longer guarded execution
   paths/block chaining and event deadlines, preserving MMU/code invalidation,
   interrupt shadows and precise fault state. These are the largest shared costs.
2. **Resolve generated-code costs before choosing its next optimization.** Named
   native helpers and anonymous frames under JIT ancestry can be attributed to
   execution, but we have not mapped those addresses to emitted guards or guest
   instructions. Approximately a quarter of Excel's samples remain at that level.
3. **Target cold/short interpreter fragments for launch workloads.** PUSH memory
   and remaining memory forms are candidates, but instruction-attempt counts
   alone cannot establish their performance value. Compilation is measurable in
   Word launch but too small in loaded Excel to explain most of its time.
4. **Keep capture/I/O outside CPU-only comparisons.** Continue to include it in
   end-to-end task results and report it explicitly. Guest disk/compression is
   not a leading bottleneck in these RAM-resident workloads.

A twofold useful-work improvement will require combined changes. Eliminating
one opcode or compilation cost alone is not supported as a route to 2×.

## Evidence and storage

The record contains all six raw host traces and both guest profiles, compressed
with verified zstd round trips; aggregate and per-run categories, source sites,
function counts, actual CPU intervals, task identities and commands are included.
The analyzer now recognizes the frame renderer, keeps unresolved frames explicit
and checks exact sample-accounting totals; six tests pass.

Existing Office and Excel parents were reused. The new reproducible Word launch
checkpoint retains about 15 MiB of compressed state and a 137 KiB disk child on
the external volume; no Office reinstall or raw large disk was made. Successful
per-run overlays and the stopped setup probe's child are removed. The Word
checkpoint's parent is the existing immutable recovered desktop disk, not the
removed probe child.

## Current bounded-batch executable: follow-up host profiles

The earlier tables above describe the memory-CALL executable, before native
PUSH and bounded batching. For the frozen current `build/native-batch/pcemu`,
three loaded Excel runs and three newly collected Word runs give:

| Main-thread sampled work | Excel recalculation | Word warm launch |
| --- | ---: | ---: |
| Dispatch/cache/validation | 44.1% | 23.7% |
| Generated code/native helpers | 30.7% | 10.9% |
| Interpreter/MMU/faults | 11.5% | 31.5% |
| Device polling/interrupts | 4.5% | 8.8% |
| Compilation | 1.7% | 13.2% |
| Capture/video/control | 4.3% | 5.4% |
| Disk/compression | 0.5% | 1.0% |
| Other host work | 2.7% | 5.5% |

The Excel collection has 12,771 task samples. Word has 1,741 task samples;
its short launch makes individual source-site attribution less certain.
Word's independently measured process CPU intervals are 667.8, 663.9 and
665.5 ms; responses are 705.7, 699.0 and 702.9 ms. These sampled runs include
profiler overhead and are not speed comparisons. All three Word restores
pass the expected screen checks. Existing snapshots and small disposable disk
children were reused; no installation media or large disk was recreated.

Dispatch is the clearest target for sustained Excel work: keeping execution
inside generated code across safe block boundaries could amortize lookup,
validation and entry/exit. Word needs separate attention to interpreted short
fragments and cold compilation. Disk compression is not a leading cost here.
Higher relative dispatch share after batching does not mean dispatch got slower;
removing polling changes the denominator. Removing all compilation alone would
barely affect loaded Excel. No single bucket establishes a twofold speedup.

The existing finer emitted-code profile separates flags from register commits
and return, but uses a larger diagnostic arena and an earlier source set. Its
percentages must not be combined numerically with this normal-arena collection.

[Excel raw traces and attribution](measurements/post-batch-host-profile-2026-10-26.json)
and [Word raw traces and attribution](measurements/word-post-batch-host-profile-2026-10-26.json)
record executable/task hashes, CPU intervals and per-run spread. Raw traces
are stored as verified zstd files. Thirteen analyzer tests pass. Production
runtime code is unchanged by this collection.

## Benchmark shutdown acceptance (26 October)

`bench_snapshot_task.py` allows `--shutdown-timeout SECONDS` (default 30,
positive and at most 3,600). This replaces a fixed five-second wait that could
expire during compressed-overlay flushing on external storage. Task response
and process CPU measurements end before shutdown; each sample separately
records `shutdownWaitMs` and `exitStatus`, and the report records the configured
limit. A nonzero emulator exit rejects the run even if task pixels matched.
Timeouts and failed exits retain guest files for inspection; successful clean
exits keep the existing narrowly scoped temporary-overlay cleanup.

Four disk-free fixture tests cover delayed clean shutdown, nonzero exit,
timeout retention and invalid limits. Run them with
`python3 -m unittest discover -s tests -p test_snapshot_task_shutdown.py`.
A real headless restored Word task also passes with exit status zero using
an existing checkpoint and a small disposable child, removed after success.
[Its report](measurements/snapshot-shutdown-2026-10-26.json) is a harness smoke
check, not a performance comparison. No emulator runtime behavior changed.

The task harness also records `nativeMemoryCalls` as a task-interval delta when
both capture states expose the counter. This excludes snapshot restoration and
pre-task idle activity; final cumulative counters must not be compared as task
costs. The optional field preserves compatibility with older executables.
The disk-free shutdown fixture verifies a known seven-call delta.
