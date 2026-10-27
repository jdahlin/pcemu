# Native segment PUSH/POP coverage, 2026-10-27

Segment stack operations now lower into native ARM64 blocks: PUSH ES/CS/SS/DS/FS/GS
and POP ES/SS/DS/FS/GS. They reuse dynamic selector reads, guarded stack accesses
and the [warm descriptor-load helper](office-segment-2026-10-27.md). No new guard
is added to ordinary cached dispatch.

## Semantics

Operand size selects the full stack transfer width; SS.B independently selects
SP/ESP addressing and advancement. Prefix segment/address overrides do not
redirect the implicit stack access. Pushes use the reference interpreter's
zero-extended selector value, including its full dword store behavior.

Pops read the full operand before interpreting its low selector word. Warm RAM
and descriptor guards precede all architectural mutation. Descriptor declines
replay the complete instruction outside the native frame, preserving the
reference exception state and device ordering. A successful descriptor load
commits the stack increment using the OLD stack width, even when POP SS changes
SS.B. POP SS ends the block and uses the existing accounted interrupt-shadow
boundary, outside cached tails and direct links. Other segment changes can
continue into subsequent native instructions using the new hidden bases.

Cold, cross-page, device, unusable-segment and possible code-write paths retain
the existing helper/replay behavior. Successful code writes exit after the
completed push. Device callbacks see the same pre/post-pop SP values as the
interpreter; a declined native probe performs no device access.

## Validation

Headless, RAM-only tests pass:

- 73,616 segment-stack checks cover code/operand/stack widths, real/protected/VM
  modes, ring-3 descriptor reads, cold/warm paging, ignored stack overrides,
  invalid descriptors, unusable SS, stack wrapping, watched writes, source and
  descriptor devices, callback-visible SP order, full-width high-word page
  faults, both SS width transitions, and accounted SS shadows in all three JIT
  block modes with/without links.
- Reference core: 354,964; segment MOV: 23,162; flag-stack: 33,512.
- Bounded batch: 112,885; cached batch: 20,421,602, including 103,944 linked
  transitions. Six profile-analyzer checks also pass.
- Both diagnostic restores and all 48 release restores pass their pixel gates
  and exit cleanly. Successful temporary overlays are removed.

Build directory: `build/office-segment-stack/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-segment-stack-tests`, `tests`, `jit-segment-tests`,
`jit-flag-stack-tests`, `jit-batch-tests`, and `jit-cached-batch-tests`.

## Coverage

One diagnostic restore per workload/build, prior PUSHF/POPF candidate versus new:

| Task | Interpreter operation | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | PUSH DS | 54,907 | 9,094 |
| Word | POP DS | 50,208 | 10,209 |
| Word | All segment PUSH/POP forms | 204,759 | 33,175 |
| Excel | PUSH DS | 93,210 | 6,030 |
| Excel | POP DS | 91,166 | 12,782 |
| Excel | All segment PUSH/POP forms | 386,611 | 43,618 |

Combined attempts fall 83.8% in Word and 88.7% in Excel. Word POP SS remains at
74 interpreted attempts in this profile; its native path is established by the
RAM-only tests, not by a claimed application counter reduction. Residual attempts
remain subject to the existing memory/descriptor guards and cache admission.
Median release native shares reach 95.36% for Word and 99.599% for Excel.

Profiles also show fewer translations (Word 82,837 → 76,536; Excel 50,302 →
45,550) and short-fragment hits (Word 233,541 → 169,728; Excel 135,410 → 98,908).
Supporting these operations changes block formation across former interpreter
boundaries as well as the individual opcode counts. These single-run diagnostic
counters are separate from release timing.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each run original-A,
original-B (the identical frozen 57ce863 executable), the prior flag-stack build,
and the candidate. All 48 release runs are retained.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,892.623 | 1,423.376 | 1,399.798 | 26.04% | 1.66% |
| Excel dependent recalculation | 7,264.624 | 6,495.587 | 6,452.646 | 11.18% | 0.66% |

Word wins seven of eight candidate/prior pairs, with a 2.18% paired median
improvement. Original A/A differences have median 0.75%, maximum 1.10%.
Excel wins three of four pairs, with a 0.66% paired median improvement;
A/A median difference is 0.25%, maximum 0.38%. These remain host-dependent results,
particularly Excel's small incremental difference.

All runs are headless and reuse existing Word/Excel snapshots with small private
overlays. No builds, tests or profilers overlap unprofiled timing. A separate GUI
VM and macOS background work remain active. CPU time excludes descheduled time
but not contention or DVFS, and includes all emulator threads and control/capture/
video work. Word's gate is cover-title readiness for the existing 4,807,680-byte
DOC, not complete pagination. Original-baseline gains include all preceding work.

[Runs, source/binary hashes, counters and test logs](office-segment-stack-2026-10-27.json)
and [compressed raw profiles](office-segment-stack-2026-10-27.profiles.zst).
Binary hashes are checked against each timed record. The compressed payload is
decompressed and compared byte-for-byte before saving the report.
