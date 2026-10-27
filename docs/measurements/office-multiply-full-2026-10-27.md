# Native implicit MUL/IMUL coverage, 2026-10-27

Implicit unsigned MUL and signed IMUL now lower for every byte/word/dword
register and memory source. This extends the old unsigned register-byte MUL
path; existing two/three-operand IMUL lowering remains in place.

## Semantics

Generated ARM64 captures both operands before modifying AX or DX, including
AL/AH and AX/DX source aliases. UMULL/SMULL computes the full product. Byte
forms replace AX; word/dword forms replace DX:AX or EDX:EAX while retaining
unwritten high halves. Unsigned overflow tests the upper product; signed
overflow compares the full product with the sign-extended low operand width.
Only CF/OF change, matching the interpreter's deterministic preservation of
all other flags. Prior lazy flags are materialized before partial flag updates.

Memory forms reuse the existing inline RAM/TLB guards and generated arithmetic.
Cold, device, cross-page or unusable-segment reads replay before translation or
data access. No data read can occur twice, and neither output register nor new
multiply flags can commit before a successful read. The read-only operation
needs no write translation or code invalidation. Its effective address can use
AX or DX because the operand is captured before either result is written.
No new cached-dispatch hot-hit check is introduced.

## Validation

Headless RAM-only suites pass:

- 129,810 new checks cover signed/unsigned multiplication at all widths and
  both code sizes, every register source including high-byte aliases, boundary
  operands and high halves, preserved flags, preceding lazy flags and following
  ADC, independent address sizes, FS/SIB addressing, cold/warm paging, source
  address aliases, PTE aliases, exact faults and callback-visible state.
- Native loops run in basic/follow/trace modes with links enabled and disabled;
  basic/follow exercise actual direct-link transitions. Trace mode can unroll
  the loop and complete through side exits without link traversal.
- Reference core: 354,964; bounded batch: 112,885; cached batch: 20,461,534,
  including 103,944 linked transitions; flag liveness: 102,230,994.

Build directory: `build/office-multiply-full/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-multiply-full-tests`, `tests`, `jit-batch-tests`,
`jit-cached-batch-tests`, and `jit-flags-tests`.

## Coverage

One diagnostic restore per workload/build, prior guarded-string candidate
versus new. F7 groups 4 and 5 identify unsigned and signed implicit products;
these are not totals for unrelated TEST/NOT/NEG/DIV operations in the group.

| Task | Interpreter attempts | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | F7 /4 MUL | 14,941 | 1,172 |
| Word | F7 /5 IMUL | 38,007 | 462 |
| Word | F6 /5 byte IMUL | 525 | 27 |
| Excel | F7 /4 MUL | 28,760 | 964 |
| Excel | F7 /5 IMUL | 2,144 | 191 |

Word's combined F7 multiply attempts fall 96.9%; Excel's fall 96.3%.
Residual interpretation includes guarded replay, cold execution and admission.
Excel's diagnostic workload did not execute byte signed IMUL; its behavior is
established by differential tests, not a claimed workload reduction.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each include original-A,
original-B (identical frozen 57ce863 executable), prior guarded-string build
and this candidate. All 48 unprofiled restores pass pixel gates and exit
cleanly; no runs are discarded. Median native share reaches 96.137% for Word
and 99.722% for Excel.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,893.238 | 1,378.661 | 1,369.427 | 27.67% | 0.67% |
| Excel dependent recalculation | 7,271.224 | 6,393.896 | 6,360.644 | 12.52% | 0.52% |

Word wins seven of eight matched pairs, with a 0.94% paired median improvement;
A/A absolute difference has median 0.30%, maximum 1.43%. Excel wins all four
pairs, with a 0.55% paired median improvement; A/A median is 0.40%, maximum
0.97%. These are modest incremental gains with host-noise limits, not evidence
that every workload speeds up by the coverage reduction percentage.

Workloads stay headless and reuse existing images with small disposable private
overlays removed on success. No builds, tests or profilers overlap release
timing. The separate GUI VM and macOS background activity remain active.
Process CPU includes all emulator threads and control/capture/video work;
it excludes restore, blocked/descheduled time, and does not eliminate
contention/DVFS effects. Word retains the unchanged 4,807,680-byte DOC and
cover-title readiness gate, which does not establish full pagination.
Original-baseline gains include preceding changes; cumulative percentages
vary with the contemporaneous controls.

## Separate Instruments evidence

After release timing, a separate headless Word restore with JIT region maps
passes the same pixel gate. Its 1,400 main-thread samples inside the verified
task window divide into generated code/helpers 26.1%, dispatch/cache/validation
22.9%, compilation 21.1%, interpreter/MMU/faults 15.8%, capture/video/control
7.4%, device polling 4.6%, other host work 1.9% and disk/compression 0.3%.
These are sample shares, not isolated function CPU durations. The trace and
map remain local under `build/office-multiply-full/word-instruments/`.

Mapped generated memory operations account for 165 self samples (11.8% of the
main-thread total); generated prologues for 60 (4.3%). String helper descendants
account for 18 samples (1.3%), plus one generated string sample. This single
trace does not establish the cause of the preceding Excel regression, but it
shows that string execution is not the largest current Word cost. Dispatch
and compilation merit attention alongside the remaining opcode gaps.

[Runs, hashes, counters, test logs and Instruments summary](office-multiply-full-2026-10-27.json)
and [compressed raw guest profiles](office-multiply-full-2026-10-27.profiles.zst)
preserve the results. Timed binary hashes are verified and compressed profile
payloads pass byte-for-byte decompression comparison. Instrumented clock times
are excluded from benchmark scores.
