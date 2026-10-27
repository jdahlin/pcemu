# Native LOOPE/LOOPNE coverage, 2026-10-27

LOOPE/LOOPZ and LOOPNE/LOOPNZ now extend the existing native LOOP/JCXZ family.
The prior candidate recorded 51,479 interpreted LOOPNE attempts in Word and
21,487 in Excel. LOOPE was not observed in these profiles; its lowering is
established by tests rather than a claimed workload reduction.

## Semantics

Conditional LOOP materializes the preceding arithmetic flags before consuming
ZF. Counter decrement does not change flags. The branch is taken only when the
post-decrement CX/ECX is nonzero and prior ZF has the required value. All flags
remain unchanged by LOOP itself, including when the ZF producer aliases CX/ECX.

Address size selects the counter width independently of operand-size target
masking. Word counters preserve upper ECX bits, including underflow; relative
byte displacements retain signed arithmetic and selected target wrapping.
Existing JCXZ/JECXZ and unconditional LOOP paths remain covered. The ordinary
cached-dispatch loop gains no new guard, and existing direct-link edges use the
computed taken/fallthrough IP.

## Validation

Headless RAM-only validation passes:

- 23,846 conditional LOOP checks cover all four counter-branch opcodes,
  code/address/operand sizes, zero and lazy ZF inputs, counter underflow and high
  halves, signed displacements, 16-bit target wrapping and instruction-fetch
  boundaries, counter/flag-producer aliases, batch bounds, all three JIT block
  modes and directly linked LOOPNE execution.
- Reference core: 354,964; bounded batch: 112,885; cached batch: 20,421,602,
  including 103,944 linked transitions. Six analyzer tests pass.
- Both diagnostic restores and all 48 release restores pass their pixel gates
  and exit cleanly. Successful private overlays are removed.

Build directory: `build/office-conditional-loop/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-conditional-loop-tests`, `tests`, `jit-batch-tests`, and
`jit-cached-batch-tests`.

## Coverage

One diagnostic restore per workload/build, prior XLAT candidate versus new:

| Task | Interpreter LOOPNE attempts | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | E0 | 51,479 | 97 |
| Excel | E0 | 21,487 | 56 |

Interpretation falls 99.8% in Word and 99.7% in Excel. Existing guards/admission
retain residual interpretation. Median release native shares reach 95.71% for
Word and 99.709% for Excel. Diagnostic clock overhead is excluded from scores.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each run original-A,
original-B (the identical frozen 57ce863 executable), the prior XLAT build and
this candidate. All 48 unprofiled runs are retained.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,885.284 | 1,379.158 | 1,375.379 | 27.05% | 0.27% |
| Excel dependent recalculation | 7,261.812 | 6,362.885 | 6,340.970 | 12.68% | 0.34% |

Word wins six of eight candidate/prior pairs, with a 0.49% paired median
improvement. Original A/A differences have median 0.50%, maximum 1.02%.
Excel wins all four pairs, with a 0.30% paired median improvement;
A/A median difference is 0.14%, maximum 0.28%. Word's small difference is within
control variation; Excel's result is modest but consistent across these pairs.

All workloads are headless and reuse existing snapshots with small disposable
overlays. No builds, tests or profilers overlap unprofiled timing. The separate
GUI VM and macOS background work remain active; process CPU excludes descheduled
time but not contention/DVFS, and includes all emulator threads and control/
capture/video work. Word's gate is cover-title readiness for the existing
4,807,680-byte DOC, not full pagination. Original-baseline gains include preceding
changes, and fresh control variation affects cumulative percentages.

[Runs, hashes, counters and test logs](office-conditional-loop-2026-10-27.json)
and [compressed raw profiles](office-conditional-loop-2026-10-27.profiles.zst).
Timed binary hashes are verified. The compressed payload passes byte-for-byte
decompression comparison.
