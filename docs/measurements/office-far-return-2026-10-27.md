# Guarded native RETF/IRET coverage, 2026-10-27

RETF, RETF immediate and IRET now use a guarded warm-RAM backend called by
ARM64. The complete frame and required segment descriptors are preflighted
before committing architectural changes. This extends coverage across real,
protected and V86 modes, including outer-privilege returns and IRET to V86.
It is a guarded C++ return backend, not fully inline ARM64 descriptor handling.

## Semantics and replay

The helper simulates each stack pop using the original SS.B and preserves
unwritten high ESP bits. Operand size independently selects each saved value.
For outer RETF, immediate cleanup applies to both the old parameter stack and
the restored stack, using each stack's own width. A word restore preserves the
high half of the intermediate ESP; dword restores replace full ESP even when
the new SS.B is zero. IRET to V86 restores all saved data selectors and full ESP.
Normal IRET retains the reference privilege masks for IOPL/IF and reserved bits.

Every required stack read must fit within one current warm permitted RAM
translation. Descriptors use existing supervisor-read probes and shared
segment decoding. Stack wrapping between pops is supported; individual reads
crossing pages conservatively replay. Cold, unusable, invalid, faulting or
device-backed accesses decline before any architectural or page-table mutation.
The interpreter then supplies exact read ordering, partial side effects and
exception delivery. V86 IRET with insufficient IOPL also replays.

After successful preflight, the helper commits SP, segment caches, flags and IP
without further faulting accesses or callbacks. Preceding cached registers and
lazy flags are flushed first, and stale registers cannot overwrite the result.
The boundary-only function slot is shared with scalar I/O, retaining fast
metadata lookup while preventing cached/direct-link execution past the return.
The ordinary block header and cached hot-hit checks remain unchanged. Every
return retains the reference device-poll boundary.

## Validation

Headless RAM-only validation passes:

- 89,428 new checks cover operand/code/old-stack/new-stack width combinations,
  real/protected/V86 modes, same/outer privilege, immediate cleanup, high ESP
  halves, 16-bit wrapping within multi-pop frames, reserved/control flags,
  V86 entry, warm/cold paging, descriptor faults, unmapped/user-inaccessible or
  unusable stacks, device-visible SP/flags/IP/clocks and repeated poll-boundary
  checks with links enabled/disabled in basic/follow/trace modes.
- Port I/O: 33,241; reference core: 354,964; bounded batch: 18,525;
  cached batch: 20,456,574 including 103,944 linked transitions;
  flag liveness: 102,230,994.

The timer ISR's IRET can now run natively; existing IRQ latency, ISR/EOI state,
clock and device-poll assertions remain intact. The batch suite performs a
few fewer boundary comparisons as a result. Cold/faulting return tests still
compare exact CPU and RAM state against the interpreter.

Build directory: `build/office-far-return/candidate`. Targets: `pcemu`,
`pcemu-profile`, `jit-far-return-tests`, `jit-port-io-tests`, `tests`,
`jit-batch-tests`, `jit-cached-batch-tests`, and `jit-flags-tests`.

## Coverage

One diagnostic restore per workload/build, prior compiled-I/O candidate versus
new:

| Task | Interpreter attempts | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | CA RETF immediate | 38,820 | 7,231 |
| Word | CB RETF | 37,675 | 1,822 |
| Word | CF IRET | 14,583 | 392 |
| Excel | CA RETF immediate | 33,152 | 4,866 |
| Excel | CB RETF | 74,537 | 2,159 |
| Excel | CF IRET | 74,962 | 234 |

Combined interpretation falls 89.6% in Word and 96.0% in Excel. Native
instruction counts remain reference instruction attempts, including guarded
replay; they do not count individual pops as separate guest instructions.
Residual interpretation includes conservative warm-frame guards and cold code.
Diagnostic clock overhead is excluded from scores.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each include original-A,
original-B (identical frozen 57ce863 executable), prior compiled-I/O build
25bbf02 and this candidate. All 48 unprofiled restores pass pixel gates and exit
cleanly; no runs are discarded. Median native shares reach 96.434% for Word
and 99.776% for Excel.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,898.966 | 1,364.912 | 1,355.997 | 28.59% | 0.65% |
| Excel dependent recalculation | 7,277.914 | 6,275.411 | 6,315.307 | 13.23% | -0.64% |

Word wins all eight matched candidate/prior pairs, with a 0.55% paired median
improvement. A/A absolute differences have median 0.68%, maximum 1.88%; the
small gain is consistent in direction with control-variation limits. Excel
wins two of four pairs, with a 0.41% paired median regression; A/A median is
0.15%, maximum 0.46%. Its median is 0.64% worse, and no Excel speedup is claimed.
Coverage alone does not guarantee performance. The mixed pairs and guest
polling/timing differences limit causal attribution of that regression.

The candidate also includes the separately committed SAHF reserved-bit fix
6b0fb27; the timed prior executable predates it. All source and executable
hashes are recorded. No claim isolates every timing difference to the return
helper alone.

Workloads remain headless and reuse existing snapshots with small disposable
private overlays removed on success. No builds, tests or profilers overlap
unprofiled timing. The separate GUI VM and macOS background work remain active.
Process CPU includes all emulator threads and control/capture/video work,
excludes restore and blocked/descheduled time, and does not eliminate
contention/DVFS effects. Word retains the unchanged 4,807,680-byte DOC and
cover-title readiness gate, not complete pagination. Original-baseline gains
include preceding changes and contemporaneous control variation.

## Separate Instruments evidence

After release timing, one mapped Excel restore passes its readiness check.
Its 6,723 task-window main-thread samples divide into dispatch/cache validation
46.4%, generated code/helpers 36.8%, capture/video/control 6.7%,
interpreter/MMU/faults 2.7%, device polling 2.5%, other host work 2.4%,
compilation 1.8%, compiled port I/O/devices 0.7% and disk/compression below 0.1%.
FarReturn helper descendants account for 11 samples (0.16%), plus one generated
FarReturn self sample. Returns are not the dominant current Excel sample cost;
this trace does not isolate or prove the cause of the timing regression.
Sample shares are not isolated function CPU durations. Instrumented clock times
are excluded from benchmark scores; raw traces/maps remain local in build output.

[Runs, hashes, counters, test logs and Instruments summary](office-far-return-2026-10-27.json)
and [compressed raw guest profiles](office-far-return-2026-10-27.profiles.zst)
preserve the evidence. Timed binary hashes are verified; compressed payloads
pass byte-for-byte decompression comparison.
