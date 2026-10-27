# Segment MOV native coverage, 2026-10-27

The [interpreter cost study](office-interpreter-cost-2026-10-27.md) identified
segment-register moves as substantial remaining work in Word and Excel.
MOV from a segment now lowers for register and memory destinations. MOV to ES,
SS, DS, FS and GS lowers for register and guarded RAM selectors, with a native
runtime helper for hidden descriptor state. CS loads and invalid segment encodings
retain the interpreter path.

## Semantics

Register destinations obey operand size: word writes preserve upper halves,
and dword writes zero-extend the selector. Memory selector operands are always
two bytes, independently of address and operand size. Selector reads are dynamic.

Real/VM loads and valid protected null selectors need no descriptor reads.
Protected nonnull loads probe the current GDT/LDT descriptor on every execution,
even for an unchanged selector. The helper accepts only existing TLB translations
and ordinary RAM. Descriptor reads use system-access semantics at ring 3. Shared
reference/native descriptor decoding preserves existing type-before-presence
checks and hidden base, limit, granularity and width behavior. The helper commits
state only after all reads and validation succeed.

Cold translations, descriptor words crossing a page, device memory, and invalid
or faulting descriptors replay in the interpreter after returning from the native frame. Selector memory operands
must independently pass the existing warm RAM read guards; otherwise replay
precedes access. A successful side-effect-free selector RAM read can be repeated
when descriptor validation declines. Descriptor probes do not walk pages, set A/D
bits, evict translations or invoke device callbacks before declining.

MOV SS terminates its native block and establishes its interrupt shadow using an
accounted CPU instruction count. Such blocks cannot acquire direct links and are
kept out of the fast cache, so cached tails return before entering them. This
avoids a new guard on every ordinary cached dispatch. Following stack operations
see the new SS width through existing context checks.

## Validation

- 23,162 segment checks cover code/operand/address widths, all selector GPRs,
  real/protected/VM modes, GDT/LDT, same-selector descriptor changes, ring-3 system
  reads, null/invalid/nonpresent descriptors, cold translations, high-word page
  faults, device replay, flags, changing data bases, cached/linked loops and
  accounted SS shadow boundaries.
- Reference core: 354,964 checks; stack-many: 4,724; bounded batch: 112,885;
  cached batch: 20,421,602, including 103,944 linked transitions.
- All workload restores are headless and use existing Word/Excel snapshots.
  Successful private overlays are removed. Profiling and timing runs are separate.

Build directory: `build/office-segment/final`. Targets: `pcemu`, `pcemu-profile`,
`jit-segment-tests`, `tests`, `jit-stack-many-tests`, `jit-batch-tests`, and
`jit-cached-batch-tests`.

## Coverage

Prior PUSHA/POPA build versus final candidate, one diagnostic restore each:

| Task | Remaining interpreter operation | Prior | Final |
| --- | --- | ---: | ---: |
| Word | MOV to segment (8E) | 211,166 | 31,420 |
| Word | MOV from segment (8C) | 59,613 | 6,918 |
| Excel | MOV to segment (8E) | 669,191 | 70,249 |
| Excel | MOV from segment (8C) | 203,195 | 9,301 |

These reductions are approximately 85%/88% for Word and 90%/95% for Excel.
Remaining attempts include guarded replay and cache/admission boundaries.

## Timing and Instruments

The final comparison uses eight Word rounds and four Excel rounds, each rotating
five executables: two identical original 57ce863 controls, the prior PUSHA/POPA
release, the initial segment prototype and the final build. All 60 final matched
restores pass, as do the 48 initial timing restores, four diagnostic restores and
one Instruments restore. Binary hashes were checked against the recorded runs.

| Task | Original CPU ms | Prior CPU ms | Prototype CPU ms | Final CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,886.272 | 1,444.525 | 1,435.802 | 1,428.365 | 24.28% | 1.12% |
| Excel recalculation | 7,273.467 | 6,576.431 | 6,581.423 | 6,517.880 | 10.39% | 0.89% |

Word wins seven of eight final/prior pairs, with a 1.12% paired median improvement.
The original A/A median difference is 0.29%, maximum 1.24%; small incremental
claims remain subject to host variation. Excel wins all four final/prior pairs,
with a 1.02% paired median improvement; A/A median difference is 0.25%, maximum
0.39%. Against the prototype, final wins five of eight Word pairs (0.44% paired
median) and all four Excel pairs (0.98%). The Excel results support removing the
hot-path check, without proving a cycle-level attribution. Median native shares
reach 94.85% for Word and 99.518% for Excel.

The first prototype added a check to the hottest cached dispatch loop to exclude
SS-changing blocks. Its initial 48 restores showed Excel 0.52% slower than the
prior build, losing all four pairs. A separate mapped Instruments trace contains
6,691 main-thread task samples: 44.3% dispatch/cache/validation, 36.3% generated
code/native helpers, 5.3% interpreter and 2.3% compilation. The segment helper
itself has seven self samples (0.10%). These samples motivate avoiding per-dispatch
overhead; they do not prove that the extra check alone caused the regression.
The final design excludes SS-changing entries during fast-cache publication.
Both prototype and final are included in the new matched comparison.

CPU time includes all emulator threads and control/capture/video work. Word's
pixel gate establishes cover-title readiness for the existing 4,807,680-byte DOC,
not complete pagination. A separate GUI VM and macOS background work remained
active; process CPU time excludes descheduled time but not contention/DVFS.
Original-baseline changes include all previous optimizations. No profiled timing
is included in the release scores and no unsuccessful timing was discarded.

[Machine-readable measurements, profiles, Instruments summary, hashes and tests](office-segment-2026-10-27.json)
and [compressed raw guest profiles](office-segment-2026-10-27.profiles.zst).
