# Guarded native far CALL/JMP coverage, 2026-10-27

Immediate far CALL/JMP (9A/EA) and memory-pointer FF /3 and /5 now use a guarded
warm-RAM transfer backend called by ARM64. Supported 16/32-bit call gates retain
same-level and privilege-changing behavior, including TSS-selected stacks and
up to 31 copied parameters. The backend is C++, not fully inline ARM64 descriptor
handling; unsupported system descriptors retain the interpreter's behavior.

## Semantics and replay

Indirect pointers use current GPRs, independently selected address width,
segment overrides and offset/selector widths. Both fields are captured before
pushes, including SP-based pointers overwritten by their own return frame.
Gate width independently selects saved values and parameter width. TSS format
selects restored SP/ESP width; each stack's SS.B selects addressing and wrapping.
Privilege-changing gate pushes use supervisor permissions while CS still holds
the caller's privilege. Target selectors, hidden caches and target offsets
follow shared segment decoding and the reference transfer rules.

Every required data/descriptor/TSS/parameter read must be an existing warm
ordinary RAM hit; every planned push needs a current dirty permitted write
translation. Individual cross-page accesses conservatively replay. Up to 35
pushes are planned before any architectural or page-table mutation. Cold,
faulting, watched or device accesses decline before side effects, then replay
with exact interpreter ordering and fault rollback. A gate can write its new
stack before a later CS fault; replay retains those RAM effects while restoring
SP and selectors.

The CS descriptor is reread after pushes in the reference implementation.
Planned writes overlapping either physical descriptor word therefore replay.
Other input aliases are safe because pointer/parameters were captured before
pushes, matching the reference. Terminal stack writes may update marked code:
precise `CPU::written` generation updates occur after each write, and no target
guest instruction executes before frame return and polling. This avoids a
needless code-bitmap fallback without reusing stale decoded instructions.

All GPRs/flags preceding the transfer are committed first. The helper commits
stack bytes, SP, optional new SS, CS and IP only after complete preflight.
Boundary-only entries retain fast metadata lookup and prevent cached/direct-link
execution past the transfer. The ordinary block header and cached hot-hit guards
remain unchanged. The boundary operand union keeps I/O/return metadata compact;
large transfer metadata is separately allocated only for transfer entries.

## Validation

Headless RAM-only validation passes:

- 60,481 new checks cover all four forms, real/protected/V86 modes,
  operand/code/address/stack widths, GDT/LDT descriptors, both gate widths,
  same/inner privilege, both TSS formats, 0/1/2/31 parameters, supervisor stacks,
  warm/cold paging, callback-visible flags/clocks/IP/SP, pointer/stack devices,
  watched writes, descriptor faults and descriptor/stack aliases.
- SP-based pointer overwrite remains native; a code-writing CALL matches exact
  generation increments. A gate's complete six-dword frame remains in RAM
  after a later CS fault restores the caller context. Repeated basic/follow/
  trace execution with links enabled/disabled retains mandatory poll boundaries.
- Far return: 89,428; port I/O: 33,241; strings: 88,295; reference core: 354,964;
  bounded batch: 18,525; cached batch: 20,456,574 with 103,944 linked transitions;
  flag liveness: 102,230,994.

Build directory: `build/office-far-transfer/final`. Targets: `pcemu`,
`pcemu-profile`, `jit-far-transfer-tests`, `jit-far-return-tests`,
`jit-port-io-tests`, `jit-string-tests`, `tests`, `jit-batch-tests`,
`jit-cached-batch-tests`, and `jit-flags-tests`.

## Coverage and original-baseline comparison

The primary comparison contains 48 unprofiled headless restores: eight Word
rounds and four Excel rounds, each rotating original-A, original-B, prior
`0e5bbd3`, and the final candidate. The two original labels use the same frozen
`57ce863` executable. All task pixel gates and clean exits pass. Existing images
and small temporary overlays were reused; no Office fixtures were recreated.

With explicit user authorization, Library process 2401 was suspended with
SIGSTOP throughout these 48 runs and resumed with SIGCONT afterward. Its stopped
and resumed process states are recorded. A preceding interrupted harness produced
no completed samples; its orphaned guest was closed and Library resumed before
the retry. Other macOS activity remained uncontrolled. Builds, tests and profilers
did not overlap scored timings.

| Task | Original CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 4,807,680-byte DOC | 2,018.32 | 1,448.93 | 1,477.97 | 26.77% | -2.00% |
| Excel dependent recalculation | 7,339.96 | 6,502.37 | 6,448.98 | 12.14% | 0.82% |

Original is the pooled median of both original labels; other columns are
per-mode medians of process user+system CPU during the task. Word response
medians are 1,502.39 ms prior and 1,531.67 ms candidate. Excel response medians
are 6,535.00 ms prior and 6,464.78 ms candidate. CPU includes all emulator threads
and control/capture work, excluding restore and shutdown. Word's cover-title
pixels establish first-page readiness, not complete pagination.

Word regresses in all eight prior/candidate pairs (0.92–3.51% more CPU). Excel
wins three of four pairs, with changes from 0.10% worse to 1.69% better.
Identical-original A/A differences reach 2.89% for Word and 3.38% for Excel;
Excel's small incremental result is therefore inconclusive. This is a coverage
extension with a measured Word regression, not an incremental speedup claim.

Separate guest profiles show far-transfer interpreted attempts falling from
67,767 to 13,614 in Word (79.9%) and 118,140 to 13,744 in Excel (88.4%). These
are 9A, EA, FF /3 and FF /5 only; other FF groups are excluded. Release native
instruction median shares rise from 96.435% to 96.500% in Word and 99.769% to
99.778% in Excel. Whole-task coverage moves little because these transfers are
rare relative to ordinary native instructions.

The report retains the 48 prototype runs and all 48 final runs measured with
Library active, alongside the primary 48 isolated runs. No completed run was
discarded. The prototype's conservative code-write fallback had no reliable
incremental win; its frozen executable and source hashes remain distinct.
Machine-readable evidence is in [the report](office-far-transfer-2026-10-27.json),
with raw guest profiles in `office-far-transfer-2026-10-27.profiles.zst`.


## Instruments and next target

Separate final-binary Time Profiler recordings with timestamped JIT maps pass
the same task gates. Library was running again during these diagnostic traces;
they are excluded from the CPU comparison. Word has 1,503 main-thread task
samples: generated code/helpers 25.5%, dispatch/cache/validation 24.1%, compilation
20.9%, interpreter 15.0%. Excel has 6,325 samples: dispatch/cache/validation 45.5%,
generated code/helpers 37.2%, interpreter 2.8%, compilation 2.1%. FarTransfer
helper descendants occupy two Word samples (0.13%) and six Excel samples
(0.09%); these small counts cannot resolve tiny helper costs reliably.

For Word, investigate admission/compilation and remaining memory-operation
fallbacks before adding another rare instruction family. Excel still primarily
needs dispatch/validation work. The profiles do not prove the cause of Word's
2% regression; a controlled admission experiment should measure both retained
coverage and end-to-end cost. Current native coverage is already 96.50%/99.78%,
so increasing the percentage alone is not evidence of better performance.
