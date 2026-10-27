# Guarded native string coverage, 2026-10-27

Immediate C6/C7 stores and ordinary memory ALU operations already lower through
native ARM64 (see [their original report](word5m-memory-alu-2026-10-26.md)).
This change extends workload coverage to MOVS, CMPS, STOS, LODS and SCAS,
including F2/F3 REP forms. Existing inline non-REP LODS/STOS remain in place.
The new backend is a guarded C++ RAM helper called by generated code, not fully
inline ARM64 string loops.

## Semantics and limits

Before modifying data, the helper validates the entire reference-sized batch
(up to 4,096 elements) against warm TLB entries, permissions, dirty write
translations, RAM bounds, A20, video apertures, watched writes and code bitmaps.
Unsafe, cold or faulting cases decline before data access and replay through
the interpreter. Preflight performs no page walks or A/D updates. A comparison
that would stop before a later unsafe page may conservatively replay too.

The helper preserves operand/address widths, segment overrides, DF, 16-bit
index wrapping and high register halves. MOVS uses memcpy only for disjoint
physical spans; overlapping spans retain directional per-element semantics.
CMPS/SCAS preserve conditional REP stopping and shared arithmetic flags.
LODS can elide intermediate ordinary RAM reads after full validation.
Writes cannot target marked code, so no code-generation update is required.

Each native REP chunk retains the interpreter's instruction accounting and
poll boundary. Partial repeats retain original pre-REP flags for a subsequent
fault, including transitions between native and interpreter chunks. The
emitter flushes preceding registers and flags before the helper and avoids
writing stale register values back afterward. No new cached hot-hit guard is
introduced.

## Validation

Headless, RAM-only suites pass:

- 88,295 string checks: all five families, REP prefixes, widths, both directions,
  address wrapping, warm/cold paging, zero and multi-batch counts, physical
  overlap, early comparison stops, partial faults, original REP flags, A20,
  device/watch/code fallback and explicit batch poll boundaries.
- Reference core: 354,964; bounded batch: 112,885; cached batch: 20,461,534,
  including 103,944 linked transitions.
- Both diagnostic restores and all 48 unprofiled release restores pass their
  pixel gates and exit cleanly. Successful private overlays are removed.

The cached test's independent traversal now follows the actual accounted
instruction boundary, since guarded strings require an early poll even on
native success; it still checks the instruction and dispatch bounds and
compares with the reference interpreter. The prior test asserting REP LODS
never becomes native now requires native execution after warmup.

Build directory: `build/office-string/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-string-tests`, `tests`, `jit-batch-tests`, and
`jit-cached-batch-tests`.

## Coverage

One diagnostic restore per workload/build, prior conditional-LOOP versus new:

| Task | Interpreted opcode | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | A5 MOVSW/MOVSD | 184,293 | 13,475 |
| Word | AF SCASW/SCASD | 92,619 | 1,539 |
| Word | A4 MOVSB | 41,029 | 6,693 |
| Word | A6 CMPSB | 6,454 | 858 |
| Excel | A5 MOVSW/MOVSD | 116,472 | 21,057 |
| Excel | AF SCASW/SCASD | 20,379 | 247 |
| Excel | A6 CMPSB | 18,988 | 489 |

Residual attempts include conservative preflight declines and warmup.
Median release native instruction share is 96.090% for Word and 99.719% for
Excel. Counts represent reference-sized instruction batches, not elements;
diagnostic timings are excluded from scores.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each include original-A,
original-B (the identical frozen 57ce863 executable), prior conditional-LOOP
and this candidate. All 48 unprofiled runs are retained.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,900.823 | 1,383.875 | 1,368.652 | 28.00% | 1.10% |
| Excel dependent recalculation | 7,286.936 | 6,347.801 | 6,400.549 | 12.16% | -0.83% |

Word wins seven of eight candidate/prior pairs, with a 1.16% paired median
improvement. Original A/A differences have median 0.58%, maximum 3.01%.
This supports a modest Word improvement with host-noise limits. Excel loses
all four pairs, with a 0.80% paired median regression; its A/A median is 0.11%,
maximum 0.22%. The Excel regression is consistent and must not be described
as a speedup. Higher native coverage alone does not guarantee lower CPU time;
helper/preflight and poll costs remain optimization targets.

All runs are headless and reuse existing snapshots with disposable small
overlays. No builds, tests or profilers overlap unprofiled timing. The separate
GUI VM and macOS background work remain active. Process CPU includes all
emulator threads and control/capture/video work, excludes restore and
blocked/descheduled time, and does not remove contention or DVFS effects.
Word uses the unchanged 4,807,680-byte DOC and cover-title readiness gate;
this does not measure full pagination. Original-baseline gains include all
preceding changes.

[Runs, hashes, counters and test logs](office-string-2026-10-27.json) and
[compressed raw profiles](office-string-2026-10-27.profiles.zst) preserve the
results. Timed binary hashes are verified; compressed profiles pass a
byte-for-byte decompression check.
