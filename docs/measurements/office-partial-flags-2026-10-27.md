# Native carry controls and SAHF, 2026-10-27

CLC, STC and CMC now lower directly alongside SAHF. These fixed-width flag
operations previously terminated native fragments. Existing LAHF, CLD/STD and
privilege-checked CLI/STI lowering remain in place.

## Semantics

CLC clears CF, STC sets it and CMC consumes and complements it. Every other
flag and all registers remain unchanged. SAHF takes SF/ZF/AF/PF/CF from AH,
masking out reserved AH bits; OF, control flags and the complete AX register
remain unchanged. Operand/address-size prefixes do not change these widths.

Prior lazy arithmetic flags materialize before a partial update. Liveness
tracks each instruction's exact written mask, retaining prior OF across SAHF
and prior CF as an input to CMC. Generated ARM64 modifies the canonical flags
image with AND/OR/XOR; no host helper or cached-dispatch guard is added.

## Validation

Headless RAM-only validation passes:

- 39,360 directed checks cover both code sizes and size prefixes, every AH byte,
  carry/control/reserved input flags, prior lazy overflow/carry, ADC/LAHF and
  interpreter SETO consumers, dead outputs followed by full writers, native
  flag branches, basic/follow/trace modes and cached/linked loops.
- Reference core: 354,964; bounded batch: 18,555; cached batch: 20,456,574,
  including 103,944 linked transitions; flag liveness: 102,230,994.

The old bounded-batch fixture used CMC as an interpreter barrier. It now uses
WAIT, retaining the same exact reference-state, dispatch and instruction-count
assertions. New tests separately require native CMC execution. CLC also no
longer splits the timer test loop, so the suite performs fewer batch-boundary
checks; the existing timer/EOI/IRQ state, native-execution and 127-instruction
latency assertions all remain in place. The native flag loop exercises actual
direct links in basic/follow modes; trace mode can unroll through side exits.
SETO remains interpreted and explicitly verifies flags at that boundary.

Build directory: `build/office-partial-flags/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-partial-flags-tests`, `tests`, `jit-batch-tests`,
`jit-cached-batch-tests`, and `jit-flags-tests`.

## Coverage

One diagnostic restore per workload/build, prior implicit-multiply candidate
versus new:

| Task | Interpreter attempts | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | F8 CLC | 41,652 | 2,780 |
| Word | F9 STC | 14,025 | 1,297 |
| Word | F5 CMC | 2,910 | 308 |
| Word | 9E SAHF | 936 | 283 |
| Excel | F8 CLC | 49,409 | 2,113 |
| Excel | F9 STC | 16,775 | 780 |
| Excel | F5 CMC | 1,369 | 226 |
| Excel | 9E SAHF | 33 | 26 |

Combined attempts fall 92.2% in Word and 95.3% in Excel. SAHF is uncommon in
these profiles; its full behavior is established by differential tests.
Warmup/admission and cold execution retain residual interpretation.
Diagnostic timings are excluded from benchmark scores.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each include original-A,
original-B (identical frozen 57ce863 executable), prior implicit-multiply build
and this candidate. All 48 unprofiled restores pass pixel gates and exit
cleanly; all runs are retained. Median native share reaches 96.208% for Word
and 99.728% for Excel.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,894.748 | 1,367.067 | 1,366.322 | 27.89% | 0.05% |
| Excel dependent recalculation | 7,281.493 | 6,351.535 | 6,357.030 | 12.70% | -0.09% |

Word wins four of eight candidate/prior pairs, with a 0.08% paired median
improvement. Absolute A/A differences have median 0.38%, maximum 1.30%.
Excel wins one of four pairs, with a 0.12% paired median regression;
A/A median difference is 0.21%, maximum 0.73%. These small incremental changes
are within control variation. No reliable performance improvement is claimed.
The change expands native coverage while retaining approximately the same
whole-task CPU cost.

All workloads are headless and reuse existing snapshots with small disposable
private overlays removed on success. No builds, tests or profilers overlap
unprofiled timing. The separate GUI VM and macOS background activity remain
active. Process CPU includes all emulator threads and control/capture/video
work; it excludes restore and blocked/descheduled time, and does not eliminate
contention/DVFS effects. Word retains the unchanged 4,807,680-byte DOC and
cover-title readiness gate, which does not measure complete pagination.
Original-baseline gains include preceding changes and contemporaneous control
variation; this change alone does not account for the cumulative reduction.

[Runs, hashes, counters and test logs](office-partial-flags-2026-10-27.json)
and [compressed raw guest profiles](office-partial-flags-2026-10-27.profiles.zst)
preserve the evidence. Timed executable hashes are verified; compressed
payloads pass byte-for-byte decompression comparison.
