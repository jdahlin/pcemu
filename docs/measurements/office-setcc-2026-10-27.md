# Native SETcc coverage, 2026-10-27

All sixteen SETcc predicates now lower for legacy byte registers and guarded
memory destinations. SETZ/SETNZ dominate observed use in Word and Excel, but
the implementation covers signed, unsigned, overflow, sign and parity forms.

## Semantics

SETcc captures its predicate from preceding guest flags, producing exactly zero
or one without changing flags. Register destinations preserve every other bit,
including AL/AH aliases of a preceding flag producer. Address and operand-size
prefixes retain independent behavior: SETcc always stores one byte. ModRM.reg
bits are ignored, matching the interpreter.

The backend reuses existing Jcc predicate generation and inverts odd conditions.
Liveness keeps preceding flags available even when a later ALU overwrites them.
For memory destinations, the captured byte survives address/translation guards
and helper calls in a stack temporary. Both inline stores and guarded slow
stores use that same value. Cold, watched, device, unusable or faulting paths
retain existing precise replay behavior. A code write commits only through
SETcc and invalidates before executing the modified next instruction. No new
cached-dispatch hot-hit guard is added.

## Validation

Headless RAM-only validation passes:

- 225,360 new checks cover all predicates and all arithmetic flag combinations,
  both code sizes and size prefixes, low/high byte destinations, ignored
  ModRM.reg fields, independent address sizes, FS/SIB addressing, warm/cold
  paging, lazy producers and dead flag outputs, callback-visible flags/clocks,
  unmapped/read-only/unusable memory, watched/video writes, code invalidation,
  batch bounds and cached/linked basic/follow/trace execution.
- Partial flags: 39,360; reference core: 354,964; bounded batch: 18,555;
  cached batch: 20,456,574 including 103,944 linked transitions;
  flag liveness: 102,230,994.

The partial-flag suite previously used interpreted SETO to observe prior OF.
It now explicitly requires that consumer to execute natively while retaining
all reference-state comparisons. Basic/follow loops exercise actual direct
links; trace mode can unroll and complete via side exits without a transition.

Build directory: `build/office-setcc/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-setcc-tests`, `jit-partial-flags-tests`, `tests`,
`jit-batch-tests`, `jit-cached-batch-tests`, and `jit-flags-tests`.

## Coverage

One diagnostic restore per workload/build, prior carry-control/SAHF candidate
versus new:

| Task | Interpreted SETcc | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | All observed conditions | 69,049 | 2,440 |
| Word | 0F 95 SETNZ | 32,518 | 1,124 |
| Word | 0F 94 SETZ | 31,886 | 817 |
| Word | 0F 9D SETGE | 3,222 | 144 |
| Excel | All observed conditions | 33,358 | 907 |
| Excel | 0F 95 SETNZ | 25,239 | 513 |
| Excel | 0F 94 SETZ | 7,631 | 232 |

Combined interpretation falls 96.5% in Word and 97.3% in Excel. Conditions
absent from workload profiles are established by differential tests, not a
claimed workload reduction. Warmup/admission and cold execution retain residual
interpretation. Diagnostic clock overhead is excluded from performance scores.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each include original-A,
original-B (identical frozen 57ce863 executable), prior partial-flags build
and this candidate. All 48 unprofiled restores pass their pixel gates and exit
cleanly; no runs are discarded. Median native instruction shares are 96.276%
for Word and 99.730% for Excel.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,894.545 | 1,370.890 | 1,356.493 | 28.40% | 1.05% |
| Excel dependent recalculation | 7,288.404 | 6,366.077 | 6,343.769 | 12.96% | 0.35% |

Word wins seven of eight candidate/prior pairs, with a 0.46% paired median
improvement. Absolute A/A differences have median 0.82%, maximum 2.62%.
Excel wins all four pairs, with a 0.37% paired median improvement; A/A median
is 0.43%, maximum 0.72%. The direction is encouraging, but these small
incremental gains have host-noise limits, especially Word's paired median
within control variation. Coverage reductions are not proportional speedups.

Workloads remain headless and reuse existing snapshots with small disposable
private overlays removed on success. No builds, tests or profilers overlap
unprofiled timing. The separate GUI VM and macOS background work remain active.
Process CPU includes all emulator threads and control/capture/video work,
excludes restore and blocked/descheduled time, and does not eliminate
contention/DVFS effects. Word retains the unchanged 4,807,680-byte DOC and
cover-title readiness gate, not full pagination. Original-baseline gains include
preceding changes and contemporaneous control variation.

[Runs, hashes, counters and test logs](office-setcc-2026-10-27.json) and
[compressed raw guest profiles](office-setcc-2026-10-27.profiles.zst) preserve
the evidence. Timed executable hashes are verified. Compressed payloads pass
byte-for-byte decompression comparison.
