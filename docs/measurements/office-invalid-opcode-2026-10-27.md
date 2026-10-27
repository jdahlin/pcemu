# Compiled invalid-ARPL fault exits

The preceding profiles ranked ARPL among the expensive interpreter leaves:
Word had 2,950 attempts, short Excel loading 1,629, and Excel recalculation
6,874. Every sampled ARPL attempt faulted, and no attempt reached the ModRM
operand decoder. Estimated shares of interpreter CPU were 8.5%, 4.4% and 17.8%;
Word/short-Excel estimates had only 17/13 samples and are imprecise. These are
shares of interpreted work, not total emulator CPU.

ARPL is invalid in real and virtual-8086 modes, as specified in
[Intel SDM Volume 2A, ARPL exceptions](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf#page=194).
Lowering ordinary protected-mode selector adjustment would not remove the
observed early faults. This change instead compiles the known mode-invalid
case into a terminal #UD exit. Ordinary protected-mode ARPL continues to use
the reference instruction path.

## Implementation and safety

The compiled block commits its prefix registers and flags and sets IP to the
faulting instruction's beginning, including supported prefixes. Its generated
frame returns a dedicated exit tag. The dispatcher accounts for the attempted
instruction and delivers #UD through `CPU::stepInvalidOpcode`, after the native
frame is gone. No reference operand decode or initial C++ exception throw/unwind
is needed for this known pre-access fault.

The interpreter and compiled exit share the existing fault-delivery sequence,
extracted into `deliverInstructionFault`. Interpreter faults still restore
their registers, segments, flags and restart IP before entering it. The new
exit already has that canonical pre-instruction state. Observer ordering,
interrupt-shadow clearing, IDT/TSS access, handler entry, error-frame handling
and delivery-failure reporting retain the same sequence.

CR0/VM context guards prevent reusing the exit in valid protected mode. A poll
boundary prevents cached batching or direct links from running into the handler
without returning to the dispatcher. TF and interrupt-shadow forced stepping
retain the reference path. Existing fetch-page/generation checks protect the
compiled bytes. The new lowering declines device-backed opcode pages, retaining
the opcode-read callback. A missing ModRM page cannot override this early #UD;
a missing opcode page still faults on fetch. Prefixes crossing two warm code
pages exercise the existing guarded boundary-entry mechanism.

Diagnostic profiles record `nativeInvalidOpcode` separately. The native
instruction counter includes these compiled fault attempts, following the
CPU instruction counter's existing attempted-instruction semantics. These
counts demonstrate removal of interpreter decode/unwind work; they do not
mean that exception delivery or ordinary protected-mode ARPL was inlined into
ARM64 code. Debug maps identify the new region as InvalidOpcode.

## Validation

`jit-invalid-opcode-tests` passes **11,212 checks** in Release. Coverage includes
real/VM86 modes, code defaults, 0–14 supported prefixes, multiple following
operand bytes, prior native ALU flags/GPRs, instruction counts above 32 bits,
opcode/ModRM fetch-fault priority, warm cross-page instruction fetch, device
opcode callbacks, complete real and VM86-to-ring-0 exception frames, explicit
absence of an error word for #UD, and batching stopping before handler execution.
It also exercises normal warmup, cached entries, three block modes with links
enabled/disabled, callbacks that clear the cache, valid-mode transitions, TF/
interrupt-shadow stepping and instruction-byte invalidation.

Regression suites pass: CPU core 354,964 checks; port I/O 33,241; flags
102,230,994; cached batches 20,456,574; profile timing 33; descriptor queries
114,774; guarded strings 88,295; far returns 89,428. String and far-return suites
also exercise the shared delivery path's restart/frame behavior. Logs are
retained in the report.

## Office measurement

Four rotated-order single-run rounds per workload compare original `57ce863`,
prior `a9ec60c` and the final candidate: **36 verified headless restores**. An
earlier 36-run cohort also passed; review then added the device-fetch guard.
Those pilot timings and hashes are retained separately and excluded from the
final table. All runs reuse existing immutable snapshots and small private
compressed disk children, removed after successful runs.

The Library VM is paused around each scored cohort and resumed in `finally`.
Our builds, tests and profiles run outside the scored intervals. Concurrent
standalone VGA-ROM work landed independently as `5e4ebc6`; it changed neither
the tested CPU/JIT sources nor the frozen binaries. Spot checks showed no
compiler/test processes, but continuously quiescent background host load is
not proven. Four samples per engine and normal guest/host variation limit
attribution of small differences. No incremental speedup is established.

Median milliseconds retain elapsed and process CPU separately. Original-relative
improvements are cumulative across earlier optimization work.

| Workload | Original elapsed / CPU | Prior elapsed / CPU | Candidate elapsed / CPU | Below original elapsed |
| --- | ---: | ---: | ---: | ---: |
| Word launch + 5 MB DOC cover | 2,158 / 2,097 | 1,557 / 1,500 | 1,531 / 1,479 | 29.1% |
| Excel launch + 650-formula XLS | 1,591 / 1,552 | 1,253 / 1,203 | 1,254 / 1,215 | 21.2% |
| Excel edit + 4,096-row recalculation | 7,707 / 7,661 | 6,673 / 6,648 | 6,681 / 6,647 | 13.3% |

Prior-relative elapsed changes are -1.7%, +0.1%, +0.1%; CPU changes are -1.4%,
+1.0%, approximately 0%. These modest differences support retaining the coverage
change without a performance-win claim. Every sample is retained, including
slower ones. Word's gate is the rendered DOC cover, Excel load verifies its ten
calculated summary values, and recalculation edits D1 in an already-open workbook.
Restore/Windows boot are excluded. Process CPU includes control/video and all
emulator threads; elapsed additionally includes scheduling and capture polling.

## Coverage and Instruments

Separate diagnostic restores passed the same pixel gates:

| Workload | Prior interpreted ARPL | Candidate interpreted ARPL | Compiled #UD exits |
| --- | ---: | ---: | ---: |
| Word load | 2,950 | 176 | 2,775 |
| Excel load | 1,629 | 144 | 1,548 |
| Excel recalculation | 6,874 | 101 | 7,095 |

All remaining interpreted ARPL attempts are warming, with zero declines or
replays. Guest timing/interrupt work varies between observed task intervals,
so candidate totals need not equal prior totals. Raw profile JSON is retained
in the adjacent zstd bundle. Profiled times are excluded from the comparison.

A separate recalculation Time Profiler run with timestamped JIT maps passed.
Its 6,793 main-thread task-window samples show dispatch/cache/validation 45.1%,
generated code/helpers 38.2%, capture/video/control 8.2%, device polling and
interrupt delivery 2.3%, compilation 1.5%, and interpreter/MMU/faults 1.5%.
Fault delivery remains real work. Dispatch and validation dominate this
already highly native workload; high native instruction coverage alone does
not imply low dispatch cost. Warming remains substantial during Word/Excel
loading, and the broader native coverage goal remains active.

Frozen local executables and commands live under `build/office-invalid-opcode/final`.
Use the existing three task JSONs with `tools/bench_snapshot_task.py --repeat 1`;
run `--guest-profile` and `--instruments --jit-debug-map` separately. Raw traces,
generated bytes, workbooks and VM media stay under ignored `build/`.
