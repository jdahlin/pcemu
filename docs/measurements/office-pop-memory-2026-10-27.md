# Native POP r/m coverage, 2026-10-27

POP r/m (8F /0) now lowers for register and memory destinations. The prior
CLI/STI candidate recorded 192,265 interpreter attempts in Excel; sampled cost
was about 3.6% of estimated interpreter CPU, not of whole-task CPU.

## Semantics

Register forms reuse the existing guarded POP implementation, including SP/ESP
destinations and mixed operand/stack widths. The memory form executes through a
paired warm-RAM runtime helper inside the native block. It does not use CPU::step
for successful guarded accesses; the pair is not yet fully inlined ARM64.

The emitter computes implicit stack addressing using SS.B. It separately forms
the destination with post-pop SP/ESP, including preserved high ESP bits when
SS.B is clear but the destination has a 32-bit address size. Normal addressing,
SIB scaling, displacements and segment overrides apply to that destination.

Before any data access, the helper checks both current translations, permissions,
write dirtiness, RAM bounds and device apertures. Watched writes and potentially
translated destination bytes decline. Probes do not walk pages, modify A/D bits,
evict TLB entries or invoke callbacks. Only after both guards succeed does the
helper capture the complete source and write the destination. This preserves
read-before-write semantics for overlapping and physically aliased operands.
The native caller commits SP after success and retains deferred arithmetic flags.

Cold/unsafe accesses replay the complete instruction outside the native frame.
This includes code writes and source/destination faults, preserving the reference
order of the stack read, SP increment, destination decoding and store. A source
that aliases the destination PTE must be captured before a write probe changes
the dirty bit; the guarded path declines such a clean translation before access.

## Validation

All checks are headless and RAM-only:

- 9,678 POP r/m checks cover code/operand/address/stack widths, every register
  destination, post-ESP SIB operands, no-base SIBs, segment overrides, wrapping,
  overlapping operands, cold/dirty translations, restricted user pages,
  supervisor/system-access behavior, stale CR3/epochs/tags, device callbacks and
  callback-visible SP order, unusable segments, source/destination page crossings,
  translated code writes, aliased PTE dirty updates and cached/linked loops.
- Reference core: 354,964; interrupt control: 11,158; bounded batch: 112,885;
  cached batch: 20,421,602, including 103,944 linked transitions.
- Both diagnostic restores and all 48 release restores pass their pixel gates
  and exit cleanly. Successful private overlays are removed.

Build directory: `build/office-pop-memory/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-pop-memory-tests`, `tests`, `jit-interrupt-control-tests`,
`jit-batch-tests`, and `jit-cached-batch-tests`.

## Coverage

One diagnostic restore per workload/build, prior CLI/STI candidate versus new:

| Task | Interpreter POP r/m attempts | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | 8F /0 | 39,335 | 3,979 |
| Excel | 8F /0 | 192,265 | 12,799 |

Attempts fall approximately 90% in Word and 93% in Excel. Guarded replay and cache
admission still account for residual interpretation. Median release native shares
reach 95.59% for Word and 99.691% for Excel.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each run original-A,
original-B (the identical frozen 57ce863 executable), the prior CLI/STI candidate
and this build. All 48 unprofiled runs are retained.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,890.977 | 1,383.426 | 1,381.556 | 26.94% | 0.14% |
| Excel dependent recalculation | 7,275.575 | 6,378.661 | 6,361.183 | 12.57% | 0.27% |

Word wins seven of eight candidate/prior pairs, with a 0.41% paired median
improvement. Original A/A differences have median 0.60%, maximum 4.15%.
Excel wins all four pairs, with a 0.36% paired median improvement;
A/A median difference is 0.31%, maximum 0.40%. These small changes do not establish
a reliable incremental speedup. The coverage gain is clear; the helper and
remaining host overhead limit what can be inferred from instruction counts.

All workloads reuse existing snapshots and small disposable overlays. No builds,
tests or profilers overlap unprofiled timing. The separate GUI VM and macOS
background activity remain active. CPU time excludes descheduled time but not
contention/DVFS, and includes all emulator threads and control/capture/video work.
Word's gate is cover-title readiness for the existing 4,807,680-byte DOC, not
complete pagination. Original-baseline gains include all preceding work; fresh
control variation also changes those cumulative percentages between reports.

[Runs, source/binary hashes, counters and test logs](office-pop-memory-2026-10-27.json)
and [compressed raw profiles](office-pop-memory-2026-10-27.profiles.zst).
Every timed binary hash is verified. The compressed payload passes a byte-for-byte
decompression round trip. Profile timings are excluded from release scores.
