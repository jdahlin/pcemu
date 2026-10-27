# Native PUSHF/POPF coverage, 2026-10-27

PUSHF/PUSHFD and POPF/POPFD now lower into ARM64 blocks using the existing guarded
stack accesses. They were frequent remaining operations in both Word and Excel
after the [segment MOV work](office-segment-2026-10-27.md).

## Semantics and validation

Operand width and SS.B are independent. PUSHF materializes prior deferred flags
before access and applies the reference interpreter's RF/VM mask to dword stores.
POPF materializes its restart flags, reads through warm RAM guards, then applies
the reference flag-change mask with current CPL/IOPL restrictions. Reserved/high
bits are retained, bit 1 is set, and SP commits only after a successful access.
VM mode checks IOPL before accessing stack memory. Disallowed operations replay
for normal interpreter fault delivery.

POPF ends the block and returns the batch before the following instruction. This
preserves a device-poll boundary after IF/TF changes. Boundary blocks cannot be
directly linked and are kept out of cached tails during fast-cache publication.
The ordinary cached-dispatch loop gains no extra guard; the outer run/batch path
carries the completion-boundary marker.

Cold, crossed-page, device and faulting POPF reads replay before access. PUSHF
uses the existing store helper for cold translations, code writes and other slow
paths; watched/device writes decline to the interpreter. A completed code write
exits after this instruction. No C++ exception unwinds through generated frames.

Headless RAM-only validation passes:

- 33,512 flag-stack checks: code/operand/stack widths, real/protected/VM modes,
  all CPL/IOPL combinations, incoming flag bits, RF/VM/reserved-bit behavior,
  cold/warm translations, user read-only pages, unusable SS, wrapping/cross-page
  accesses, watched writes, devices, faults, translated code writes and exact
  POPF poll boundaries in Basic/FollowJumps/Trace modes, with and without links.
- 354,964 reference core checks and 23,162 segment checks.
- 112,885 bounded batch checks and 20,421,602 cached batch checks, including
  103,944 linked transitions.

Build directory: `build/office-flag-stack/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-flag-stack-tests`, `tests`, `jit-segment-tests`,
`jit-batch-tests`, `jit-cached-batch-tests`.

## Coverage

One diagnostic restore per build/workload, prior segment candidate versus current:

| Task | Interpreter operation | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | PUSHF | 119,006 | 5,809 |
| Word | POPF | 118,477 | 10,046 |
| Excel | PUSHF | 383,881 | 3,774 |
| Excel | POPF | 377,848 | 5,964 |

Combined attempts fall approximately 93% in Word and 99% in Excel. Remaining
attempts include guarded replay and cache/admission boundaries. Diagnostic timer
costs are excluded from the release timing comparison.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each include original-A,
original-B (the identical frozen 57ce863 executable), the prior segment build and
the candidate. All 48 release restores and both diagnostic restores pass their
pixel gates and exit cleanly; no run is discarded.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,897.226 | 1,430.262 | 1,417.856 | 25.27% | 0.87% |
| Excel dependent recalculation | 7,279.132 | 6,521.897 | 6,497.039 | 10.74% | 0.38% |

Word wins seven of eight candidate/prior pairs, with a 0.85% paired median
improvement. Original A/A differences have median 0.44% and maximum 4.18%.
Excel wins three of four pairs, with a 0.51% paired median improvement;
A/A median difference is 0.19%, maximum 0.26%. These are modest favorable
incremental results, with a noisy Word control pair and no universal per-run win.
Median release native shares reach 95.09% for Word and 99.570% for Excel.

All runs are headless. Existing Word/Excel snapshots are reused with disposable
small overlays removed after success. No builds, tests or profilers run during
unprofiled timing. A separate GUI VM and macOS background work remain active;
CPU time excludes descheduled time but not contention or DVFS. Process CPU
includes all emulator threads, control/capture and video work. Word's gate is
cover-title readiness for the 4,807,680-byte legacy DOC, not full pagination.
Original-baseline improvements include all preceding changes.

[Measurements, source/binary hashes, counters and test logs](office-flag-stack-2026-10-27.json)
and [compressed raw profiles](office-flag-stack-2026-10-27.profiles.zst).
The compressed payload is decompressed and compared byte-for-byte. Recorded
binary hashes are verified against each executable before generating the report.
