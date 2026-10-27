# Native XLAT coverage, 2026-10-27

XLAT now lowers as a guarded byte lookup in ARM64 blocks. The previous candidate
recorded 44,552 interpreter attempts in Word and 111,641 in Excel.

## Semantics

The effective offset adds zero-extended AL to BX/EBX, then applies the selected
16/32-bit address-size wrapping before adding the segment base. Operand-size
prefixes do not change the byte lookup. DS is the default; segment overrides
retain their ordinary meaning. Only AL changes: AH, upper EAX and all flags are
preserved. The index uses the cached AL value after preceding native writers.

The existing inline read guards require an ordinary RAM hit with current
translation/permissions and valid segment usability. A20 masking is retained.
Cold/unsafe reads replay before translation or data/device access, so a replay
cannot duplicate callbacks or expose page-walk changes before refetching the
instruction. No new runtime helper or ordinary cached-dispatch guard is added.

## Validation

All tests are headless and RAM-only:

- 17,697,596 XLAT checks cover every AL value, multiple upper-EAX and arithmetic
  flag patterns, BX/EBX boundary values, 16/32-bit code/address sizes, ignored
  operand prefixes, every segment override, real/protected/VM modes, cold/warm
  paging, A20 and 32-bit wrapping, cached AL writers, flag-dependent branches,
  device and fault replay, and chained cached/linked lookups with changing AH.
- Full RAM comparisons occur per matrix context; individual iterations compare
  GPR/control state, page-table RAM and lookup-byte preservation.
- Reference core: 354,964; memory double-shift: 23,348; bounded batch: 112,885;
  cached batch: 20,421,602 with 103,944 linked transitions. Six analyzer tests pass.
- Both diagnostic restores and all 48 release restores pass their pixel gates
  and exit cleanly. Successful private overlays are removed.

Build directory: `build/office-xlat/candidate`. Targets: `pcemu`, `pcemu-profile`,
`jit-xlat-tests`, `tests`, `jit-memory-double-shift-tests`, `jit-batch-tests`, and
`jit-cached-batch-tests`.

## Coverage

One diagnostic restore per workload/build, prior double-shift candidate versus new:

| Task | Interpreter XLAT attempts | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | D7 | 44,552 | 123 |
| Excel | D7 | 111,641 | 64 |

Interpretation falls 99.7% in Word and 99.94% in Excel. Remaining attempts retain
the existing memory and admission guards. Diagnostic clock overhead is excluded
from release timing.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each run original-A,
original-B (the identical frozen 57ce863 executable), the prior double-shift
candidate and the new build. All 48 unprofiled runs are retained.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,896.264 | 1,374.795 | 1,369.897 | 27.76% | 0.36% |
| Excel dependent recalculation | 7,293.314 | 6,356.593 | 6,341.079 | 13.06% | 0.24% |

Word wins five of eight candidate/prior pairs, with a 0.20% paired median
improvement. Original A/A differences have median 1.27%, maximum 1.82%.
Excel wins all four pairs, with a 0.22% paired median improvement;
A/A median difference is 0.33%, maximum 0.46%. The coverage gain is clear, but
these small differences do not establish a reliable incremental speedup. Median
release native shares reach 95.66% for Word and 99.706% for Excel.

All workloads reuse existing snapshots and small disposable overlays. No builds,
tests or profilers overlap unprofiled timing. The separate GUI VM and macOS
background work remain active; process CPU excludes descheduled time but not
contention/DVFS, and includes all emulator threads and control/capture/video work.
Word's gate establishes cover-title readiness for the existing 4,807,680-byte DOC,
not full pagination. Original-baseline gains include all preceding work; fresh
controls affect cumulative percentages between reports.

[Runs, source/binary hashes, counters and test logs](office-xlat-2026-10-27.json)
and [compressed raw profiles](office-xlat-2026-10-27.profiles.zst).
Timed binary hashes are verified. The compressed payload passes byte-for-byte
decompression comparison.
