# Word and Excel: guarded native ENTER coverage, 2026-10-27

Generated ARM64 now calls a guarded C++ ENTER backend for operand widths 16/32 and all nesting
levels 0–31. The latest preceding profiles identify 17,771 declined ENTER
attempts in Word and 9,260 in Excel. This implements the current CPU reference's
frame-building rules, including independently selected operand and stack widths.

## Ordered preflight

The helper simulates each push and frame-chain read with local BP/SP values.
Operand width controls saved values and BP arithmetic; SS.B controls push and
allocation addressing. Narrow updates preserve their untouched high bits. The
saved frame address is captured after the initial push, and the nesting byte is
masked to five bits. Allocation adjusts SP without probing the reserved bytes,
as in the existing reference.

Up to 32 writes are planned before any RAM or architectural mutation. Each read
must be a warm permitted ordinary RAM hit and each write additionally needs an
already-dirty translation. Planned reads overlay earlier planned writes in byte
order, including partial overlaps and physically aliased virtual addresses.
Pre-reading the whole chain from original RAM would be incorrect.

Watched or marked-code writes, device/unmapped accesses and fields crossing a
page boundary replay before any access with observable effects. The interpreter retains exact fault
ordering and earlier frame writes when a later read/push faults and BP/SP roll
back. Marked-code writes conservatively replay, preserving generation updates
and preventing stale native continuation. Successful preflight commits frame
bytes, then BP/SP. Flags remain unchanged.

Generated code commits preceding dirty GPRs/flags before the helper and reloads
BP/SP afterward. Following native memory operations and LEAVE therefore see the
new frame. Cached and directly linked blocks remain supported. No ordinary block
header, IR operand record or memory-operand metadata grows, and ENTER needs no
per-operation heap allocation.

ENTER at either end of a short fragment uses normal warmup. A regression test
failed before the admission adjustment; at warmup 8, a two-instruction fragment
now produces 186 native instructions across 100 invocations instead of waiting
for cheap-fragment promotion.

## Validation

37,835 new headless RAM-only checks pass:

- Code/operand/stack widths, real/protected/V86 modes, ring-3 permissions,
  cold/read-warm/write-warm paging, allocation 0/1/1234h/FFFFh, all 256 raw
  nesting bytes, and ignored address/segment overrides.
- Complete and partial overlapping frame chains, physical aliases, high virtual
  BP chains with both SS.B values, A20 masking, and conservative crossing-field
  replay.
- Later read/push faults with earlier RAM writes retained; exact BP/SP/flags
  rollback; flags before a fault even with a later full flag writer.
- Watched and MMIO read/write callback order and visible instruction state;
  precise generation updates during code-writing replay.
- Dirty BP/SP inputs, native ENTER/LEAVE continuation, repeated stack-width
  changes, cached multi-block execution and actual promoted direct links.

Other passing suites: core 354,964; bounded batch 18,525; cached batch 20,456,574
with 103,944 linked transitions; liveness 102,230,994; two-page fetch 4,760;
far-pointer loads 95,760; far transfer 60,481; guest profiling 33. Build:
`build/office-enter/candidate`; frozen measured binaries: `build/office-enter/final`.

## Coverage

Separate final-binary headless profiles pass the existing task gates and clean
exits. ENTER interpreter attempts fall from 22,734 to 5,276 in Word (76.8%) and
11,844 to 2,843 in Excel (76.0%). Declined ENTER attempts fall to zero in both.
The remaining candidate counts are 5,090 warming/186 replay in Word and 2,672
warming/171 replay in Excel. Prior counts belong to `c59397e`. Diagnostic timings
are excluded from scoring.

## Original-baseline comparison

All 48 unprofiled headless restores pass: eight Word and four Excel rounds,
each rotating identical original-A/B, prior `c59397e`, and candidate. Original
remains frozen `57ce863`. All runs and executable hashes are retained. Existing
snapshots and small temporary compressed overlays were reused; no Office
fixtures were recreated.

With existing user authorization, Library PID 2401 was SIGSTOP-suspended during
scoring and SIGCONT-resumed afterward. Other host activity and CPU frequency
remained uncontrolled. No builds, tests, profiling or trace analysis overlapped
scoring.

| Task | Original CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M DOC | 2,303.82 | 1,690.52 | 1,696.96 | 26.34% | -0.38% |
| Excel recalculation | 8,117.66 | 7,081.67 | 7,045.75 | 13.20% | 0.51% |

Original is the pooled median of both identical-original labels; the other CPU
columns are per-mode medians of process user+system time during the task. CPU
includes all emulator threads and control/capture work, excluding restore and
shutdown. Word response medians are 1,770.94 ms prior and 1,765.45 ms candidate;
Excel response medians are 7,143.08 ms prior and 7,120.59 ms candidate.

Word is slower in six of eight pairs, with changes from 3.12% worse to 2.14%
better. Excel wins three of four pairs, ranging from 0.53% worse to 0.98% better.
Identical-original A/A variation reaches 2.02%/1.14% in Word/Excel. These small
incremental medians do not establish a reliable speedup. Release native-share
medians rise from 96.560% to 96.586% in Word and 99.814% to 99.816% in Excel;
the targeted opcode diagnostics provide the clearer coverage evidence.

Word's gate establishes cover-title readiness for the 4,807,680-byte DOC, not
complete pagination. Excel uses the existing dependent recalculation fixture.
Machine-readable task reports, source/executable hashes, tests and pause/resume
evidence are in [the report](office-enter-2026-10-27.json). Raw coverage profiles
are archived in `office-enter-2026-10-27.profiles.zst`.

## Instruments and remaining work

Separate final-binary Time Profiler traces with timestamped JIT maps pass the
same gates after Library resumes. Word has 1,675 main-thread task samples:
generated code/helpers 30.0%, dispatch/cache/validation 23.5%, compilation
16.8%, interpreter 13.7%. Excel has 7,099 samples: dispatch/cache/validation
43.2%, generated code/helpers 37.4%, interpreter 2.4%, compilation 2.3%. No
samples resolve to the EnterFrame region in these short traces; that does not
establish zero cost.

Word's load interval and Excel's edit/recalculation interval are different
workloads. Both reports retain elapsed task-response and process CPU time; the
comparison table above uses CPU time. Both exclude VM restore and Windows boot. Word includes Word application
launch plus opening the DOC; Excel starts with an already-open workbook. Future summaries should retain
these workload and metric labels when comparing results.

ENTER coverage is now useful on the measured workloads, but no incremental
speedup is established. Bit-scan/descriptor fallbacks and Word's warming/
compilation work remain; Excel's dominant dispatch/validation cost persists.

## Elapsed-time view

The user-facing wait metric is elapsed task-response time, which is recorded
separately from process CPU. Word measures application launch plus opening the
DOC until its cover title appears. Excel measures changing **D1 from 3 to 4**
and obtaining the dependent result; it is not an Excel launch/load benchmark.

| Timed workload | Original elapsed ms | Prior elapsed ms | Candidate elapsed ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word launch + DOC cover | 2,383.28 | 1,770.94 | 1,765.45 | 25.92% | 0.31% |
| Excel edit + recalculation | 8,174.76 | 7,143.08 | 7,120.59 | 12.90% | 0.31% |

These small prior-relative elapsed differences also do not establish a reliable
speedup. The CPU and elapsed columns answer different questions and should
remain explicitly labelled. Host cache is warm; guest cache comes from the
checkpoint. The timing boundary remains a rendered pixel gate, not complete
background pagination or an application-idle guarantee.
