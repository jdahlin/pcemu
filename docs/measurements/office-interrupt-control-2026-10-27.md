# Native CLI/STI coverage, 2026-10-27

CLI/STI now lower directly into ARM64 blocks. The prior segment-stack profiles
recorded 183,723 interpreter attempts in Word and 1,155,377 in Excel. Most sampled
executions completed normally, making these useful remaining coverage targets.

## Semantics

The emitter computes CPL using the same protected/VM context as the reference
CPU, then checks current IOPL at runtime. This remains a runtime guard when a
cached block is reused after POPF changes IOPL. Denied instructions replay outside
the native frame, preserving prefix IP, prior registers/flags, CR2 and normal
#GP delivery. Flag liveness retains the arithmetic image needed by that replay,
even if a later native ALU instruction would overwrite it.

CLI clears only IF and can continue into ordinary native blocks. STI sets only
IF and writes the interrupt shadow through its own completed instruction count.
Its native block terminates there and stays outside cached tails/direct links,
using the same accounted-boundary mechanism as SS loads. The shadow count uses
a full 64-bit load/add/store and includes preceding instructions in the frame.
The following instruction retains reference interpretation under the shadow.
Repeated STI with IF already set keeps the reference CPU's existing behavior.
No additional guard is added to the ordinary cached-dispatch loop.

## Validation

All validation is headless and uses RAM-only kernels or existing guest fixtures:

- 11,158 interrupt-control checks cover real/protected/VM modes, every CPL/IOPL
  combination, IF initially clear/set, ignored prefixes, paging, cached IOPL
  changes, precise privilege faults, flags live across later writers, 64-bit
  instruction counts, shadow boundaries in all three block modes, native/linked
  CLI loops and batch bounds.
- Board-level PIC cases verify that IRQ0 remains pending at STI and arrives
  only after the protected instruction; CLI suppresses pending delivery.
- Reference core: 354,964; segment stack: 73,616; flag stack: 33,512; bounded
  batch: 112,885; cached batch: 20,421,602 with 103,944 linked transitions.

The old bounded-batch test used CLI as an unsupported-instruction barrier. That
case now uses CMC, retaining its stop-batching assertions; new CLI-specific tests
separately establish native continuation and interrupt behavior.

Build directory: `build/office-interrupt-control/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-interrupt-control-tests`, `tests`, `jit-segment-stack-tests`,
`jit-flag-stack-tests`, `jit-batch-tests`, `jit-cached-batch-tests`.

## Coverage

One diagnostic restore per workload/build, prior segment-stack candidate versus new:

| Task | Interpreter operation | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | CLI | 116,325 | 2,727 |
| Word | STI | 67,398 | 9,689 |
| Excel | CLI | 782,227 | 1,937 |
| Excel | STI | 373,150 | 7,100 |

Combined attempts fall about 93% in Word and 99% in Excel. Remaining attempts
include privilege-fault replay and cache/admission boundaries. Diagnostic timer
costs are excluded from release scores.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each run original-A,
original-B (the identical frozen 57ce863 executable), the prior segment-stack
build and the candidate. All 48 release restores and both diagnostic restores
pass their pixel gates and exit cleanly; every run is retained.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,896.695 | 1,393.076 | 1,379.746 | 27.26% | 0.96% |
| Excel dependent recalculation | 7,257.964 | 6,475.573 | 6,371.007 | 12.22% | 1.61% |

Word wins five of eight candidate/prior pairs, with a 0.95% paired median
improvement. Original A/A differences have median 0.74%, maximum 2.53%; treat
Word's small incremental result cautiously. Excel wins all four pairs, with a
1.56% paired median improvement; A/A median difference is 0.19%, maximum 0.61%.
Median release native shares reach 95.55% for Word and 99.676% for Excel.

All release and diagnostic restores are headless, reuse existing Word/Excel
snapshots and remove successful private overlays. No builds, tests or profilers
overlap unprofiled timing. The separate GUI VM and macOS background activity
remain running. Process CPU excludes descheduled time but not contention/DVFS,
and includes all emulator threads and control/capture/video work. Word's gate
measures cover-title readiness for the existing 4,807,680-byte DOC, not completed
pagination. Original-baseline gains include all preceding optimizations.

[Runs, hashes, counters and test logs](office-interrupt-control-2026-10-27.json)
and [compressed raw profiles](office-interrupt-control-2026-10-27.profiles.zst).
Every timed binary hash is checked against the executable, and the compressed
profile payload passes a byte-for-byte decompression round trip.
