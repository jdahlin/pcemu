# Native SHLD/SHRD coverage, 2026-10-27

SHLD and SHRD now share a result/flag generator for register and guarded memory
operands, covering immediate and CL counts at word/dword widths. The previous
candidate supported register SHRD only. Excel's remaining immediate SHLD count
and sampled interpreter CPU cost exceeded XLAT's in the prior profile, making
this the next shared coverage target.

## Semantics

Count bytes are masked to five bits. A combined 32/64-bit operand pair implements
both directions, including the reference CPU's deterministic 16-bit overshifts.
Source/count/destination aliases are captured before modifying registers. Results
preserve upper halves for word register writes. Shared flags match the reference
carry, sign, zero and parity results, count-one overflow, AF treatment and the
reference's undefined-flag behavior for larger counts.

Memory nonzero counts use existing inline warm-RAM read/modify/write guards.
They require valid translations and write dirtiness, reject watched/device/code
writes and page crossings, then capture the old value and commit result/flags
without callbacks or possible guest faults. Cold/unsafe paths replay before data
translation/access, preserving accessed/dirty ordering and self-PTE operands.

Unlike ordinary single shifts, zero-count double shifts do not load or store the
operand. They still perform reference segment-usability validation. A separate
native zero path therefore bypasses data translation and write guards after the
segment check, preserving flags and avoiding MMIO/watch callbacks even for an
unmapped or read-only operand. Register immediate zeros remain no-ops.

## Validation

Headless, RAM-only validation passes:

- 23,348 memory double-shift checks cover code/operand/address widths, both
  directions and count sources, masked zero/one/width/overshift counts, source
  register/address aliases, segment overrides, cold/warm paging, no-access zeros,
  unusable segments, devices, watches, read-only/unmapped pages, code writes,
  aliased PTE updates, live flag consumers and cached/linked loops.
- 102,230,994 flag-liveness checks include all SHLD/SHRD register overlaps,
  every count byte, multiple operand patterns, live/dead flags and fault exits.
- Reference core: 354,964; bounded batch: 112,885; cached batch: 20,421,602,
  including 103,944 linked transitions.
- Both diagnostic restores and all 48 release restores pass the task pixel gates
  and exit cleanly; successful private overlays are removed.

The register pair-admission test formerly used a rotate as an unsupported barrier.
Since rotates now lower, it uses HLT instead, retaining the same pair-size and
warmup assertions. The register direction matrix is extended to SHLD as well.

Build directory: `build/office-double-shift/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-memory-double-shift-tests`, `jit-flags-tests`, `tests`,
`jit-batch-tests`, `jit-cached-batch-tests`.

## Coverage

One diagnostic restore per workload/build, prior POP r/m candidate versus new:

| Task | Interpreter operation | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | SHLD immediate (0F A4) | 15,966 | 1,079 |
| Excel | SHLD immediate (0F A4) | 120,493 | 1,033 |

Immediate SHLD attempts fall approximately 93% in Word and 99% in Excel. Other
forms are covered by the tests, without attributing a large workload counter
reduction to already-supported register SHRD. Guarded replay and cache admission
retain residual interpretation. Diagnostic timings are excluded from release scores.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each run original-A,
original-B (the identical frozen 57ce863 executable), the prior POP r/m build and
the candidate. All 48 unprofiled runs are retained.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,888.472 | 1,381.978 | 1,383.091 | 26.76% | −0.08% |
| Excel dependent recalculation | 7,269.925 | 6,364.141 | 6,372.909 | 12.34% | −0.14% |

Word wins four of eight candidate/prior pairs; paired median timing is 0.03%
higher. Original A/A differences have median 0.89%, maximum 2.32%.
Excel wins one of four pairs; paired median timing is 0.15% higher;
A/A median difference is 0.09%, maximum 1.50%. This run does not demonstrate an
incremental speedup. The small timing differences remain uncertain; the change
is retained for verified coverage, not a claimed performance gain. Median release
native shares reach 95.61% for Word and 99.699% for Excel.

All workloads are headless and reuse existing snapshots with small private
overlays. No builds, tests or profilers overlap unprofiled timing. The separate
GUI VM and macOS background work remain active; process CPU excludes descheduled
time but not contention/DVFS, and includes all emulator threads and control/
capture/video work. Word's gate is cover-title readiness for the existing
4,807,680-byte DOC, not complete pagination. Original-baseline improvements include
all earlier changes, and fresh control variation affects cumulative percentages.

[Runs, hashes, counters and test logs](office-double-shift-2026-10-27.json)
and [compressed raw profiles](office-double-shift-2026-10-27.profiles.zst).
Timed binary hashes are verified, and the compressed payload passes byte-for-byte
decompression comparison.
