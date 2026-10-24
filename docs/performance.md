# Performance measurements

Measured on the local Apple M4 Mac, 2026-10-24. These results do not establish
Rosetta-like Windows application performance.

## Comparable register arithmetic loop

Run `python3 tools/bench_rosetta.py` on Apple Silicon with Rosetta installed.
The source includes opaque assembly functions so the compiler cannot fold away
the arithmetic. Each loop performs 48 dependent 32-bit increments, a comparison,
and a branch. Both native kernels return the checked accumulator (240,000,000).
The ARM64 and Rosetta results are medians of five warmed runs. PCEmu executes
250 million guest instructions in each of its interpreter and JIT modes.

| Engine | Million equivalent guest instructions/second |
| --- | ---: |
| Native ARM64 assembly | 4,169 |
| x86-64 assembly through Rosetta | 4,152 |
| PCEmu AArch64 register JIT | 1,244 |
| PCEmu interpreter | 54.7 |

The register JIT is about 3.3 times slower than Rosetta for this narrow pattern.
PCEmu's loop ends at a host instruction budget, while the native assembly loop
ends at the accumulator limit; they have comparable arithmetic and branch work,
but are not identical programs. These measurements exclude guest memory access,
privilege changes, paging faults, devices, graphics, disk I/O, and startup.

The raw local result is `build/rosetta-register-benchmark.json`. The interpreter
and JIT in the general benchmark print their actual accumulator and native
instruction count for verification.

## Current whole-system limitations

Register blocks can be translated with paging enabled, with physical page,
permissions, and TLB epoch validation. Short/near direct branches terminate native blocks and use their committed
flags. Most memory operations and indirect control flow still execute in the interpreter. Cold addresses warm up before compilation;
very short blocks remain interpreted to avoid native entry overhead. A direct
lookup cache reduces dispatch cost, and an arena-recycling regression checks its
pointer lifetime.

Windows boot and application throughput have not been compared with an equivalent
Rosetta workload. The dominant next performance work is safe memory lowering and
control flow, with precise exits for faults, interrupts, and self-modifying code.

After adding direct branches, 249,999,650 of 250 million instructions in the
register loop executed natively. Throughput remained within the previous
measurement range; native branch coverage alone did not provide a measured
speed gain. Raw output: `build/rosetta-branch-results.log`.

Native blocks now load only referenced guest GPRs, spill only written GPRs,
and save only the host callee-save pairs they actually use. Differential tests
pass, but the latest register-loop measurement (1,244 million/s) does not
establish a throughput improvement over the earlier 1,316 million/s run.
The latest raw output is `build/rosetta-register-traffic-results.log`; the
pre-change result is preserved in `build/rosetta-before-register-traffic.json`.

## Guarded memory-move backend

`./build/pcemu --benchmark-memory --benchmark-steps 50000000` runs sixteen
load/add/store sequences and a backward branch per loop. All sixteen cells are
checked against the exact loop count. Flat and paged guest RAM use the same
guest bytes; interpreter and JIT checksums both equal 16,326,528. On the local
M4 with other emulator instances suspended during measurement:

| Workload | Interpreter million instructions/s | JIT million instructions/s | Speedup |
| --- | ---: | ---: | ---: |
| Flat RAM | 57.3 | 404.3 | 7.05x |
| Paged RAM | 41.5 | 283.2 | 6.83x |

Raw output: `build/memory-fixed-width-bench.log`. These are synthetic memory loops,
not Windows application or Rosetta-equivalence measurements. Native blocks
now include 16/32-bit MOV memory operands (including moffs, address-size and
segment overrides). A guarded helper accepts single-page RAM and preserves
MMU permissions/A20. MMIO, faults, cross-page accesses and watched stores exit
to one precise interpreter instruction. Code-changing stores end the block.

Effective addresses and source values are passed from cached ARM64 registers.
Successful accesses retain cached registers and deferred flags; cold guard exits
commit the exact prior state for interpreter replay. Fixed-width RAM copies
allow the host compiler to emit direct loads/stores inside the helper. Page-fault replay, partial cross-page stores, MMIO side-effect
counts and in-block self-modification have dedicated regressions, alongside
512 differential address/segment/operand-size/paging combinations.

The initial Windows memory-backend integration reached Explorer and completed
a guest restart with exact file survival (`build/win95-memory-restart`). That
run retired about 19% of its instructions natively, including later idle time;
its elapsed time is not comparable with older interactive runs. Cache profiling
now exposes compilation/recycling counts. Unchanged-code TLB flushes preserve
cached translations after checking permissions and physical mapping, avoiding
unnecessary redecoding; remap and fetch-permission regressions remain enforced.

## Memory comparison with Rosetta

`python3 tools/bench_rosetta.py --memory --blocks 10000000` compares the same
sixteen independent 32-bit load/add/store sequences on native ARM64, x86-64
under Rosetta, and the original PCEmu guest. Native assembly uses a decrement
and conditional branch to bound execution; PCEmu uses a backward jump with
its external instruction budget. Rates count 49 equivalent guest operations
per iteration, excluding the extra native loop-counter instruction. All sixteen
cells are checked; the checksum is 160,000,000. Native kernels use ordinary
host memory without guest segmentation, paging, code invalidation or devices.
Their reported time is the median of five runs; PCEmu is a single timed run
per mode. These are throughput measurements, not application comparisons.

| Backend | Million equivalent guest instructions/s |
| --- | ---: |
| Native ARM64 | 13,134.8 |
| Rosetta x86-64 | 11,778.4 |
| PCEmu flat RAM JIT | 387.7 |
| PCEmu paged RAM JIT | 291.3 |

Evidence: `build/rosetta-memory-benchmark.json` and
`build/rosetta-memory-benchmark.log`. The flat RAM JIT is about 30 times slower
than Rosetta on this pattern. Register-only throughput does not predict memory
performance; reducing per-access helper costs and dispatch remains necessary.

## Inline RAM and TLB hits

The inline path removes the C++ helper from ordinary RAM accesses. Repeating
the ten-million-iteration comparison produces the following checked results:

| Backend | Million equivalent guest instructions/s |
| --- | ---: |
| Native ARM64 | 13,778.4 |
| Rosetta x86-64 | 12,131.5 |
| PCEmu flat RAM JIT | 1,659.2 |
| PCEmu paged RAM JIT | 839.5 |
| PCEmu flat interpreter | 59.2 |
| PCEmu paged interpreter | 41.3 |

Evidence: `build/rosetta-inline-memory-benchmark.json` and `.log`. Flat RAM is
28.0x faster than the interpreter and paged RAM 20.3x. Relative to the preceding
helper backend, these runs improve flat RAM about 4.3x and paged RAM about 2.9x.
Rosetta remains about 7.3x faster on the flat pattern and 14.5x faster than the
paged guest. The native kernels omit guest MMU semantics, as described above;
these results still do not establish application performance parity.

## Cached pointers and compact guards

Native memory blocks now retain the RAM pointer in x28 and, when a guest GPR
slot is unused, retain the code-bitmap pointer in that callee-saved slot.
CPU-local TLB addresses use ADD immediates relative to x19. For page-aligned,
power-of-two RAM sizes, the existing cross-page check permits a shift/branch
bounds guard; arbitrary RAM sizes retain the full comparison.

The same ten-million-iteration comparison measures 2,077.9 million guest
instructions/s flat and 923.4 million/s paged. Interpreter rates are 58.7 and
41.3 million/s (35.4x/22.4x speedups). Native ARM64 measures 13,392.4 million
equivalent guest instructions/s and Rosetta 12,352.1 million/s. Rosetta remains
about 5.9x faster than flat PCEmu and 13.4x faster than its paged guest on this
pattern. Evidence: `build/rosetta-hoisted-memory-benchmark.json` and `.log`.
The earlier scope restrictions still apply; whole-application parity remains
unproven.

Specializing interpreter RAM loads/stores for 1/2/4-byte widths raises the
checked flat interpreter loop from 58.7 to 68.6 million guest instructions/s
and paged interpretation from 41.3 to 44.9 million/s in a subsequent run. JIT
throughput remains about 2.09 billion/s flat and 0.924 billion/s paged.
Evidence: `build/rosetta-fixed-interpreter-memory-benchmark.json` and `.log`.
The live Setup guest was suspended only during measurement and resumed by a
finally block. These remain synthetic throughput measurements.

## Snapshot task regression

The fresh OSR2 install now has real guest checkpoints at Explorer and at an
open Calculator displaying 0. Its 64 MiB RAM image compresses to 5.9 MiB;
saving takes about 60 ms and a live restore about 53 ms. The calculator
checkpoint's disk overlay is 103 bytes, referencing an existing immutable
APFS-shared backing. Preview images are compressed as well.

`tools/bench_snapshot_task.py` runs independent restores, sends the recorded
input, checks before/after LCD pixels, and discards successful runs' private
COW files and temporary frames. It records all samples and retains failures
alive. Five restores verified 2+2=4 with the JIT and five with the interpreter.
Median process-to-ready latency was 51.5 ms (JIT) / 50.9 ms (interpreter).
Median control-to-result latency was 119.2 ms / 81.5 ms. These include process,
keyboard, capture and polling overhead, and are upper bounds rather than
isolated guest execution time. The first JIT process had a 352.5 ms cold start;
all samples are retained. Evidence: `build/snapshot-calculator-bench/result.json`
and `build/snapshot-calculator-interpreter/result.json`.

This small task is currently slower with a cold JIT cache than when interpreted.
That regression is now reproducible without repeated boot or manual setup.
Broader opcode lowering and dispatch/compilation costs remain the next
whole-guest optimization targets.

Byte-register and byte-memory lowering passes 177,436 differential checks,
including AH–BH preservation, page/segment faults, MMIO and self-modifying
code. Five new Calculator restores all produced 4. Median ready latency was
49.5 ms and task response 107.1 ms, compared with the prior JIT's 119.2 ms.
These short end-to-end samples include host scheduling and capture overhead;
the interpreter baseline remains faster at 81.5 ms. Evidence:
`build/byte-jit-calculator-bench/result.json`.

## Office 97 startup

The original Office 97 Professional SR-1 installer completes in Windows 95.
Separate post-boot fixtures launch Word and Excel from Run to a pixel-verified
rendered status bar. Five samples per configuration give these medians:

| Engine | Word | Excel |
| --- | ---: | ---: |
| Initial JIT | 2.522 s | 1.184 s |
| Native LEA and stack operations | 2.210 s | 1.048 s |
| Interpreter | 2.214 s | 0.979 s |

The startup improvements are about 12% / 11%. Word's native coverage increases
from 18.7% to 31.3%. Execution-batch time falls from 2.216 s to 1.944 s on Word;
the end-to-end figures also include control, captures and polling. Host caches
are not flushed, while guest RAM/file-cache state and cold JIT caches repeat
from the same checkpoints. Matching a status bar excludes splash screens but
does not establish completion of all background work. Word now roughly matches
the interpreter; Excel remains slower with the JIT. Rosetta parity is unproven.

Read-only mapping of compressed disk headers cuts median checkpoint readiness
from about 1.06 s to 0.24–0.25 s without expanding disk payloads. First-run
outliers remain in the raw results. See [Office benchmark commands](office97.md)
and `build/office97-startup-summary.json` for evidence and limitations.

## Whole-startup profiling and SDL comparison

Host stack sampling and a separate opcode/dispatch build identify interpreter
execution (~45%), JIT dispatch (~16%) and polling (~10%) as the main costs.
Disk/compression is below 1%; the observation harness itself is significant.
Normal unsupported JIT decoding had used C++ exceptions. Replacing those
routine exits and eliminating redundant PIC scans improves an alternating,
unprofiled five-pair **SDL** comparison from 2.524 to 2.209 s for Word and
1.210 to 1.023 s for Excel (12.5% / 15.4%). These are different frontend
conditions from the earlier headless tables, so compare each measured pair
within its own configuration. Execution-batch time also improves.

See [the profiling report](startup-profiling.md) for complete call-tree
attribution, opcode gaps, observer overhead, sample-boundary checks, worker
thread waits and reproduction commands. Evidence:
`build/profile-comparison-sdl/result.json`. All 541,340 checks and the Win3.11
save-file / DOS reset smoke tests pass on this version.

## Why native instruction coverage is not whole-guest speedup

After native INC/DEC, a separate five-second macOS host sample of a long,
fixed-count CoreMark diagnostic is retained in `build/incdec-host-profile/sample.txt`.
The sampled run passes guest CRC/return-to-DOS checks; its timings are excluded
from performance comparisons. Of 4,279 main-thread samples, 1,626 (~38%) are on
the interpreted-dispatch path, 1,038 (~24%) in machine polling and its device
children, and 787 (~18%) have Jit::run itself at the top of the stack. These are
sampling observations for this workload, not universal CPU-time percentages.
Native generated blocks and their helpers account for much of the remainder.

The same workload executes about 79% of guest instructions natively. That count
omits dispatch/device overhead and does not weight expensive interpreted
instructions. It must not be treated as a native share of execution time.
Generated blocks still return through the host dispatcher, spill/load guest
registers and materialize flags through C++ helpers. Machine polling currently
checks keyboard/mouse/PIT/IRQ state after every dispatch, including tiny blocks
and single interpreted instructions.

Larger gains therefore require reducing execution boundaries: event-deadline
based device service, native block links with bounded interrupt/fault exits, and
less eager flag/helper work. Additional opcode coverage remains useful, but its
instruction percentage alone does not predict speedup. The earlier register-only
benchmark's ~23x JIT/interpreter ratio and CoreMark's ~2–3x ratio measure different
workloads. Neither establishes 10–100x acceleration of a complete Windows guest.

## Deadline guards for idle device work

PIT tick checks now test the counter's next edge before entering its out-of-line
update. Periodic counters wait for the next reload boundary; one-shot counters
avoid repeat updates after their edge, while preserving backward-clock handling.
Keyboard/mouse service skips the polling body when the output buffer is empty,
queued input is not ready and no mouse sample is due. It uses existing live queue,
controller and mouse-deadline fields: input injection, command replies, guest I/O
and restored snapshots require no new deadline-cache invalidation or snapshot
format. Button transitions already queued by the mouse remain immediate.

This is a per-device fast path, not a complete event scheduler. Machine::poll,
PIC scans and interrupt-delivery checks still run after every dispatch. Full
batching and native block links remain separate work.

`build/event-deadline-controlled/build.json` records source-frozen runners with
identical common sources/compiler options and variants only for these guards.
All six final CoreMark intervals use the same COM bytes and 4,000 iterations,
with 1,590,037,601 guest instructions and 1,262,986,108 native instructions each.
Three alternating pairs yield baseline median 295.5 and guarded median 317.4
iterations/host-second. Paired ratios are 1.0694, 1.0740 and 1.0678: median about
6.9% improvement. Every interval passes upstream runtime/CRC validation and DOS
return; raw data remains under `build/event-deadline-controlled/`.

The separate five-second profile at
`build/event-deadline-host-profile/sample.txt` observes 802/4,271 samples (~19%)
in machine polling/device children, versus 1,038/4,279 (~24%) in the preceding
INC/DEC profile. These are sampled observations; profiled timings are excluded
from the controlled comparison.

`build/event-deadline-tests.log` passes the full suite, including timer mode,
gate, null-count, reload and rewind boundaries, controller pacing, command reply
priority, immediate button/input injection and IRQ-enable transitions. DOS CRCs
and 28 micro intervals pass (`build/event-deadline-smoke.log`); Win3.11 saves exact
Notepad contents (`build/event-deadline-win311.log`). Updated rtts and installed
OSR2 cold boots/restarts pass at `build/win95-smoke-35fw89x7/result.json` and
`build/win95-smoke-422r2bqj/result.json`. Word/Excel pixel-task results are in
`build/event-deadline-{word,excel}/`. Passing runs removed their private disks.

Interpreter dispatch guards now omit native-only RAM pointer, code bitmap,
write-watch and stack-width validation for negative and warming cache entries.
Those paths execute the interpreter against current CPU state; translation
captures the current native assumptions when warmup completes. Code generation,
execution epoch, segment/mode and mapping validation remain immediate, including
for negative entries, so replacing unsupported bytes with supported code does
not wait for a periodic retry. The read-JIT regression exercises that transition.
This is a reduction in dispatcher checks, with no measured speedup claimed yet.
Validation: `make test` (`build/dispatcher-guards-tests.log`) and
`make smoke-guest-perf` (`build/dispatcher-guards-smoke.log`) pass, including
CoreMark CRCs, all 56 JIT/interpreter micro intervals, matching instruction
counts, return to DOS and intentional wrong-result rejection.

A subsequent frozen-source comparison (`build/dispatcher-controlled/build.json`
and `result.json`) changes only `jit.cpp`, comparing the parent of `2d8670a`
with that commit's dispatcher guards. Three alternating CoreMark pairs all
validate canonical CRCs, run for at least ten seconds and return to DOS. Each
run executes 1,590,037,601 guest instructions, including 1,262,986,108 native
instructions. Median rates are 318.523 baseline and 320.164 candidate iterations
per host second; paired candidate/baseline ratios are 1.00244, 1.01671 and
1.00257. The median paired gain is only 0.26%, too small to distinguish reliably
from host variation with this sample. This does not establish a meaningful
whole-workload improvement; native block boundaries and flag/helper overhead
remain higher-priority optimization work. All six private disk children were
removed after validation.

Logical flag materialization for native OR/AND/XOR/TEST now stays in generated
ARM64 code. It computes operand-width ZF/SF and low-byte parity, clears CF/OF,
preserves unrelated flags, sets the fixed bit, and matches the interpreter's
deterministic AF convention even though x86 leaves AF undefined for these
operations. All sixteen conditional branches are also emitted directly when
they consume these flags or enter a block with already materialized flags.
That change left arithmetic flag producers on their existing helper. Memory fault exits
and INC/DEC carry dependencies still materialize the preceding flags before
replaying or continuing an instruction.

The byte-JIT suite adds every combination of OF/CF/ZF/SF/PF against all sixteen
branch predicates, with distinct taken and fallthrough targets, plus logical
operand-width and flag preservation checks. The full suite passes in
`build/native-logical-flags-tests.log` (1,872,421 byte-JIT checks).

The frozen-source comparison in `build/native-logical-flags-controlled/`
changes only `jit.cpp` against `22259a5`. Three alternating CoreMark pairs all
pass canonical CRCs, exceed ten seconds and return to DOS. Every run executes
1,590,037,601 guest instructions with 1,262,986,108 native instructions. Median
rates are 318.233 baseline and 327.905 candidate iterations per host second;
paired ratios are 1.03039, 1.03565 and 1.03446 (median gain 3.45%). This measures
cheaper native execution, not increased instruction coverage. It is a guest
CoreMark result, not evidence of the same gain for Windows applications or of
Rosetta parity. Passing runs removed their private compressed disk children.

Runtime compatibility passes DOS CRCs and all 56 JIT/interpreter micro intervals
(`build/native-logical-flags-smoke.log`), exact Win3.11 Notepad file contents
(`build/native-logical-flags-win311.log`), rtts Win95 cold boot/restart
(`build/win95-smoke-47t38uzd/result.json`) and installed OSR2 cold boot/restart
(`build/win95-smoke-yi34o65h/result.json`). Both Windows 95 runs observe one
guest-requested firmware reset followed by Explorer. Word and Excel restore
and task-pixel checks pass in `build/native-logical-flags-{word,excel}/`.
These are compatibility checks, not application timing comparisons or a new
installer run. Private test disks are removed after success; a source/evidence
manifest is retained at `build/native-logical-flags-validation.json`.

Native arithmetic flag materialization extends that path to ADD, SUB, CMP and
register INC/DEC. Addition computes a zero-extended 64-bit sum so the carry out
of a 32-bit operand is available; subtraction tests unsigned borrow. Signed
overflow uses the operand/result sign relationships at the actual operand
width. INC/DEC update OF/SF/ZF/AF/PF while preserving the materialized CF. The
existing flag-dependency ordering and memory-fault exits remain in place.
Conditional branches following these operations also stay in generated code.
ADC/SBB and other unsupported operations still use interpreter fallback.

The full suite passes in `build/native-arithmetic-flags-tests.log`, including
1,887,397 byte-JIT checks. New arithmetic tests cross carry, borrow, auxiliary
carry and signed-overflow boundaries at 8/16/32 bits against every Jcc predicate
with distinct branch targets, and check exact flags against the interpreter.

The initial frozen comparison in `build/native-arithmetic-flags-controlled/`
changes only `jit.cpp` against `9d5ff3e`. Three alternating calibrated pairs
pass canonical CRCs, exceed ten seconds, return to DOS and remove their private
disks. Median rates are 330.655 baseline and 348.126 candidate iterations per
host second, with paired ratios 1.06811, 1.04854 and 1.05284 (median gain 5.28%).
Automatic calibration selects 4,000 iterations for baseline and 6,000 for
candidate, so the absolute instruction counts differ in this comparison.

A second comparison (`build/native-arithmetic-flags-fixed/result.json`) uses
the same frozen executables with 6,000 iterations in all six independent DOS
runs. Every run passes canonical CRCs, exceeds ten seconds and executes exactly
2,385,055,829 guest instructions with 1,894,469,612 native instructions. Median
rates are 330.009 baseline and 349.041 candidate iterations per host second;
paired ratios are 1.06184, 1.05540 and 1.06156 (median gain 6.16%). These fixed
runs include initial JIT warmup and compilation, whereas automatic calibration
warms the code before its final interval. Both comparisons retain all samples.
Neither establishes application-wide gains or Rosetta parity.

Runtime compatibility passes DOS CRCs and the 56-interval micro matrix
(`build/native-arithmetic-flags-smoke.log`), exact Win3.11 Notepad contents
(`build/native-arithmetic-flags-win311.log`), rtts Win95 boot/restart
(`build/win95-smoke-avow4uql/result.json`) and installed OSR2 boot/restart
(`build/win95-smoke-ls6k7hd_/result.json`). Both Win95 runs observe one reset
and a second Explorer desktop. Word/Excel snapshot task pixels also pass in
`build/native-arithmetic-flags-{word,excel}/`; these are compatibility checks
and do not establish application speedups or repeat the installer.
Passing compatibility and benchmark runs remove their private test disks.
`build/native-arithmetic-flags-validation.json` records source hashes and
the evidence paths.

Guest benchmark profiling is available with
`python3 tools/bench_guest_perf.py --guest-profile --iterations 1000` (or
`--suite micro --loops 17`). It builds `build/guest-bench-profile` and writes
one counter/IP-sample JSON per measured interval, referenced from each result
sample. Counters reset at BEGIN and stop at END, excluding DOS boot, calibration
and checksum verification outside those intervals. Instrumented binaries always
identify themselves as profiled, and the runner excludes their CoreMark timings
from score eligibility. Microbenchmark rates in such results are diagnostic
too. Normal runners reject a profiling request instead of silently omitting it.

`make smoke-guest-profile` checks both engines on CoreMark and the full 28-case
micro matrix. It verifies interval instruction boundaries, interpreted attempts
against guest-minus-native counts, dispatch counters, sampled IPs, CRCs/checksums,
identical guest work across engines, diagnostic labels and private disk cleanup.
Both this check (`build/guest-profile-smoke-final.log`) and the normal benchmark
smoke (`build/guest-profile-normal-smoke.log`) pass.

After native flag lowering, the 1,000-iteration diagnostic in
`build/coremark-guest-profile-steady/` executes 397,509,732 guest instructions,
315,737,936 natively, with 81,771,796 interpreted attempts. IMUL (`0F AF`) is
11.58% of interpreted attempts; immediate register shifts (`C0/C1/D1`) total
about 18.3%. Some already-supported operations remain interpreted because
unsupported neighbors end blocks before the production minimum block length.
The `0F` prefix bucket overlaps extended opcode counts; do not sum both.
These counts identify coverage opportunities, not time spent per opcode.

A separate uninstrumented host sample at
`build/native-flags-host-profile/sample.txt` has 4,275 main-thread samples:
1,717 (40.2%) in the interpreted dispatch subtree, 808 (18.9%) in machine/device
polling, and 863 (20.2%) with `Jit::run` at the top of the stack. These are
diagnostic stack proportions, not speed comparisons; sampled run timings are
excluded. The counter run averages only 4.16 guest instructions per native
call and 2.52 instructions per dispatch overall. Extending blocks across IMUL
and shifts can therefore also amortize dispatch and polling, while bounded
native block links and event scheduling remain larger structural work.

The JIT now lowers register-source IMUL in its `0F AF`, `69` and `6B` forms,
at 16/32-bit operand widths with operand-size overrides. ARM64 SMULL produces
the full signed product; comparing it with the sign-extended truncated result
sets CF/OF as specified in Intel's
[IMUL reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf).
Other flags follow the interpreter's deterministic preservation convention for
the architecturally undefined flags. Pending arithmetic flags are materialized
before IMUL, and subsequent carry consumers/fault exits observe its new CF/OF.
The low product replaces only the destination operand, preserving upper words
and aliased source semantics. Memory-source and one-operand IMUL remain on the
interpreter path.

`build/native-imul-tests.log` passes the full suite, with 2,094,827 byte-JIT
checks. New coverage includes all register pairs, three encodings, signed
overflow boundaries, sign-extended immediates, both code/operand widths,
pending logical/arithmetic flag producers, all Jcc predicates and fault replay
after a carry-consuming INC.

The frozen comparison at `build/native-imul-controlled/` changes only
`jit.cpp` against `ca07999` and runs 6,000 CoreMark iterations per independent
DOS boot. All six samples pass canonical CRCs, exceed ten seconds, execute
2,385,055,829 guest instructions and return to DOS. Baseline executes
1,894,469,612 instructions natively (79.43%); candidate executes 2,085,773,535
(87.45%). Median rates are 346.933 baseline and 451.013 candidate iterations
per host second. Paired ratios are 1.33493, 1.30000 and 1.27539, giving a median
paired gain of 30.0%. The coverage gain exceeds the IMUL count alone because
blocks can now include adjacent supported instructions. This establishes a
CoreMark improvement, not the same application-wide gain or Rosetta parity.

The diagnostic profile at `build/native-imul-guest-profile/` uses the same
1,000-iteration COM hash as `build/coremark-guest-profile-steady/`. Guest
instructions stay at 397,509,732; interpreted attempts drop from 81,771,796 to
49,887,873 (39.0%), and dispatches from 157,629,140 to 130,561,245 (17.2%).
Average guest instructions per native call rise from 4.16 to 4.31, and per
dispatch overall from 2.52 to 3.04. These are coverage/boundary counts; profiled
timings are excluded from performance comparisons.

Compatibility passes DOS CRCs and all 56 micro intervals
(`build/native-imul-smoke.log`), exact Win3.11 Notepad file contents
(`build/native-imul-win311.log`), rtts Win95 cold boot/restart
(`build/win95-smoke-dyholej1/result.json`) and installed OSR2 cold boot/restart
(the result directory is recorded in `build/native-imul-osr2.log`). Word/Excel
task pixels pass in `build/native-imul-{word,excel}/`. Passing runs remove their
private disks. `build/native-imul-validation.json` records source hashes and
evidence; these checks do not repeat the installer or establish application
startup gains.

The JIT now emits register SHL/SAL, SHR and SAR for immediate counts and the
implicit-one encodings (`C0/C1/D0/D1`) at 8/16/32-bit operand widths. It masks
counts to five bits and treats zero as an instruction with no flag/register
effects. Carry extraction handles the operand-width boundary; SAR sign-fills
for larger counts. CF/SF/ZF/PF and count-one OF are computed in ARM64 code.
For architecturally undefined AF, multi-bit OF and oversized logical-shift CF,
the result matches the interpreter's existing deterministic convention.
Partial flag producers materialize preceding flags before changing their own
bits. Memory operands, rotates and CL-count encodings retain interpreter fallback.
The architectural reference is SAL/SAR/SHL/SHR in the local Intel SDM text,
instruction reference volume 2B, pages 4-600 onward.

`build/native-shift-tests-final.log` passes the full suite, including 2,957,189
byte-JIT checks. Added cases cover every register (including high-byte aliases),
both code widths and operand overrides, signed/value boundaries, masked and
oversized counts, the implicit-one encodings, every Jcc predicate, pending
ADD flags, carry-consuming INC and a following precise memory-fault exit.

`build/native-shift-controlled/` freezes a common source tree and changes only
`jit.cpp` against `081d59e`. Each of the six independent DOS runs executes
10,000 CoreMark iterations, passes canonical CRCs, exceeds ten seconds and
returns to DOS. Guest instructions are identical at 3,975,092,643. Native
instructions rise from 3,476,295,000 (87.45%) to 3,728,178,916 (93.79%). Median
rates are 450.805 baseline and 636.680 candidate iterations per host second;
paired ratios are 1.44113, 1.42345 and 1.38671, giving a median paired gain of
42.34%. This measures the effect on guest CoreMark, including block extension
across shifts, and does not establish application-wide gains or Rosetta parity.

The matched 1,000-iteration profiles (`build/native-imul-guest-profile/` and
`build/native-shift-guest-profile/`) use the same guest COM hash and instruction
count. Interpreted attempts fall from 49,887,873 to 24,700,049, and dispatches
from 130,561,245 to 101,971,099. Average guest instructions per native call rise
from 4.31 to 4.82, and per dispatch overall from 3.04 to 3.90. Instrumented
timings are diagnostic only. These short native blocks still leave substantial
scope for cross-instruction flag elimination and native block linking.

Compatibility passes DOS CRCs and all 56 micro intervals
(`build/native-shift-smoke.log`), exact Win3.11 Notepad contents
(`build/native-shift-win311.log`), rtts Win95 cold boot/restart
(`build/win95-smoke-z79wmxkq/result.json`) and installed OSR2 cold boot/restart
(`build/win95-smoke-6zll5op6/result.json`). Word/Excel snapshot task pixels pass
in `build/native-shift-{word,excel}/`. These checks do not repeat the installer
or establish application startup gains. Passing runs remove their private
disks; `build/native-shift-validation.json` records source hashes and evidence.

The JIT now analyzes flag liveness backward across each decoded block. It
tracks CF/PF/AF/ZF/SF/OF separately, including INC/DEC's preserved CF, IMUL's
preserved non-CF/OF flags and shifts' preserved AF/multi-bit OF. Flag producers
whose outputs are all overwritten can omit their flag calculations; a dead
register CMP/TEST omits its entire computation while retaining architectural
instruction accounting. A skipped partial flag writer leaves any earlier
pending producer available for later preserved-bit dependencies.

All flags remain live at block exits and before memory operations. Memory
comparisons also retain their output flags for an exit after code invalidation.
This conservative boundary preserves fault replay and code-change state; it
does not yet defer partial-flag reconstruction onto cold memory-exit paths.
ADC/SBB remain interpreter-only and would require explicit flag-input analysis
when introduced to the JIT.

`make test` includes the new `build/jit-flags-tests` target: 5,000 deterministic
mixed arithmetic/multiply/shift/compare sequences at both code widths with
operand overrides, plus memory-fault and successful code-invalidating-store
boundaries before a later flag overwrite. It passes 50,096 checks, and the full
suite passes in `build/flag-liveness-tests-complete.log`.

`build/flag-liveness-controlled/` changes only `jit.cpp` against `0e08cde` in a
common frozen tree. All six independent 10,000-iteration CoreMark runs validate
canonical CRCs, exceed ten seconds and return to DOS. They execute identical
guest/native instruction counts (3,975,092,643 / 3,728,178,916), compile 277
blocks during the interval and never recycle the code arena. Median rates are
650.653 baseline and 664.656 candidate iterations per host second; paired
ratios are 1.03851, 1.02401 and 1.01609 (median observed gain 2.40%). Resident
aligned arena occupancy after each run falls from 712,384 to 700,848 bytes
(1.62%), including previously compiled boot code. This is a small measured
CoreMark gain; conservative memory barriers limit the available dead-flag work.
The new `residentNativeBytes` sample field reports current arena occupancy,
which resets on recycle/clear rather than accumulating all historical code.

Compatibility passes normal DOS benchmark checks and diagnostic profiling
checks (`build/flag-liveness-{smoke,profile-smoke}.log`), exact Win3.11 Notepad
contents (`build/flag-liveness-win311.log`), rtts Win95 cold boot/restart
(`build/win95-smoke-kj6zh5z5/result.json`) and installed OSR2 cold boot/restart
(directory recorded in `build/flag-liveness-osr2.log`). Word/Excel task pixels
pass in `build/flag-liveness-{word,excel}/`. These concurrent checks establish
compatibility, not application speed comparisons or a new installer run.
Passing test disks are removed; `build/flag-liveness-validation.json` records
source hashes and evidence.

A deferred partial-flag prototype was evaluated after that pass, but is not
enabled in the shipped backend. It retained per-instruction reconstruction
inputs in a larger native frame and materialized flag recipes at exits. The
full suite, including 64-instruction blocks, passed. In the frozen comparison
`build/deferred-flags-controlled/`, all six 10,000-iteration CoreMark runs pass
CRCs and DOS return with identical guest/native instruction counts. Paired
ratios are 0.99743, 0.99369 and 1.00577 (median change -0.26%); resident code
grows from 700,848 to 712,288 bytes. This establishes no meaningful speedup.
The prototype remains at `build/deferred-flags-prototype.cpp` for reference;
the modularized backend retains the previous tested flag-emission behavior.

The JIT source split is documented in `docs/architecture.md`. Dispatch/cache
management stays in `jit.cpp`; decoding, shared typed IR, flag analysis, guarded
runtime helpers and ARM64 emission have explicit module boundaries. The full
suite passes in `build/jit-modules-tests-final.log`, including 50,146
flag-liveness checks and the added long-block coverage. This refactor is not
presented as a performance improvement.

Refactor validation passes normal and profiled DOS smoke suites
(`build/jit-modules-{smoke,profile-smoke}.log`), exact Win3.11 Notepad contents
(`build/jit-modules-win311.log`), rtts Win95 boot/restart
(`build/win95-smoke-dptwj6zy/result.json`), installed OSR2 boot/restart
(`build/win95-smoke-tbbuilsl/result.json`) and Word/Excel snapshot task pixels
(`build/jit-modules-{word,excel}/`). A 10,000-iteration CoreMark compatibility
run (`build/jit-modules-coremark/result.json`) matches the pre-split backend's
3,975,092,643 guest instructions, 3,728,178,916 native instructions, 277 compiled
blocks, zero arena recycles and 700,848 resident native-code bytes. Its timings
are excluded from comparisons because compatibility checks ran concurrently.
Decoder and emitter extraction audits confirm the same logic apart from named
IR kinds and the extracted analysis function. Passing runs remove their private
disks; `build/jit-modules-validation.json` records source hashes and evidence.


### Register NOT experiment (2026-10-24, not retained)

A trial lowered register F6/F7 `/2` NOT to ARM64 MVN with width-preserving
insertion, including AH–BH. NOT leaves incoming and pending arithmetic flags
unchanged. The full unit suite passed, including 91,106 flag checks with mixed
NOT/ALU blocks and width overrides. An initial decoder overlap with TEST was
caught by the existing read tests and corrected before these measurements.

Three alternating before/after pairs ran the same 10,000-iteration CoreMark
program in DOS, with valid upstream CRCs and successful return to DOS. Each
measured interval exceeded 15 seconds. Raw local evidence is in
`build/native-not-comparison/`, with runner hashes and all sample results in
`comparison.json`. The frozen baseline binary predates the experiment; the
candidate differs in NOT lowering and its tests. Timing variation and unrelated
host activity remain possible, so these samples do not establish a causal
regression or an improvement.

| Pair | Before seconds | NOT seconds | Before / NOT |
| --- | ---: | ---: | ---: |
| 1 | 16.709717 | 15.998339 | 1.04447 |
| 2 | 15.478177 | 15.848665 | 0.97662 |
| 3 | 15.608991 | 16.342087 | 0.95514 |

The median paired ratio is 0.97662. Guest instructions match at 3,975,092,643;
native instructions rise from 3,728,178,916 to 3,730,218,909, only 0.0513
percentage points of total coverage. Resident code grows from 700,848 to
702,464 bytes. With no demonstrated benefit, the implementation and its
feature-dependent tests were removed. The committed JIT remains the modular
baseline; this experiment is not a shipped optimization.


### Bounded same-page jump stitching (2026-10-24, opt-in)

`--jit-follow-jumps` keeps GPRs and pending flags in one generated block across
unconditional direct jumps. Formation remains limited to one linear/physical
page and 64 executed guest instructions, including backward jumps. Conditional
branches still terminate blocks. Default execution remains in basic-block mode.

Three alternating mode comparisons used one frozen runner binary (SHA-256
`5766d0254b91a01dd202afc46c406ae03cf7967e7fd2a744edd120f07a488bdf`), the
same 10,000-iteration program, valid upstream CRCs, and successful DOS return:

| Pair | Basic seconds | Joined seconds | Basic / joined |
| --- | ---: | ---: | ---: |
| 1 | 15.337071 | 14.568708 | 1.05274 |
| 2 | 20.840752 | 17.179828 | 1.21309 |
| 3 | 16.346379 | 14.912445 | 1.09616 |

The median paired ratio is 1.09616, an observed 9.6% throughput improvement.
The wide range and host timing variation limit the precision of this estimate;
it is not a general application or Rosetta comparison. Raw results, program
hashes, load averages and runner hashes remain in
`build/joined-jumps-comparison/`. Guest instructions match at 3,975,092,643;
native instructions increase from 3,728,178,916 to 3,758,905,372. Arena occupancy
grows from 700,848 to 758,512 bytes (8.2%).

Separate diagnostic 1,000-iteration profiles execute the same 397,509,732 guest
instructions. Dispatches/device polls fall from 101,971,099 to 97,379,695 (4.5%);
native calls fall from 77,271,050 to 75,752,046 (2.0%), and interpreted attempts
fall from 24,700,049 to 21,627,649. These profiles explain the boundary reduction;
their timings are excluded from the performance comparison.

The full unit suite passes; the final 60,502 flag checks cover joined forward
and backward jumps in 16/32-bit code, the instruction budget, terminal targets,
precise faults and code-changing stores. All 28 DOS microkernel intervals pass.
Opt-in compatibility checks pass Win3.11 Notepad exact-file verification, rtts
Win95 and installed OSR2 cold boot/restart, and Word/Excel snapshot task pixels.
This does not rerun either Windows installer. Evidence and source hashes are
listed in `build/joined-jumps-validation.json`; successful private disks were
removed. `smoke_win311.py`, `smoke_win95.py` and `bench_snapshot_task.py` accept
`--jit-follow-jumps` to reproduce these checks.


### Fresh joined-block OSR2 installer verification (2026-10-24)

The opt-in mode subsequently completes a fresh DOS-to-Windows 95 OSR2 install,
including two Setup-requested firmware resets in one process, Calculator 2+2=4,
exact Notepad file persistence across another restart, and an independent cold
boot/restart. `build/win95-joined-installer/result.json` and
`build/win95-smoke-qsm7rud0/result.json` contain the evidence; the status document
records its scope. All temporary disks were removed after success. This is a
correctness check, with no before/after installer timing claim. A short host
sample from hardware detection is retained as diagnostic evidence only.


### Headless application checks and default joining (2026-10-24)

Five rotated trials per application compare basic blocks, joined blocks and the
interpreter with one frozen emulator binary and the unchanged VGA launch
snapshots. All 30 headless launches produce the expected pixels and discard
private disks. The user requested headless runs; an earlier completed SDL set
is retained separately and is not pooled into these numbers. Future automated
x86 validation is headless unless explicitly requested otherwise.

| Task | Basic median ms | Joined median ms | Interpreter median ms | Median paired basic/joined ratio |
| --- | ---: | ---: | ---: | ---: |
| Word | 1425.1 | 1430.0 | 2058.4 | 1.00383 |
| Excel | 702.9 | 665.4 | 928.0 | 1.04366 |

Word is effectively unchanged. Excel's paired median indicates an observed
4.4% gain; individual paired ratios range from 0.99019 to 1.10031. Word's
ratios range from 0.87892 to 1.01052, so no Word improvement is claimed.
All raw samples, including outliers and per-run load averages, remain in
`build/joined-office-headless/`. These response times include control/capture
latency and are upper bounds, not isolated CPU time. Separately accumulated
dispatch/device-poll wall-time medians are 1228.3/1226.8/1774.9 ms for Word and
605.4/578.2/802.5 ms for Excel (basic/joined/interpreter). The fixtures are the
older VGA workloads, not the newer integrated high-resolution desktop, and
this does not establish Rosetta-level application performance.

With these results, the preceding CoreMark boundary reduction, and successful
fresh OSR2 installation, same-page unconditional jump joining is now the default
for the normal emulator and DOS benchmark runner. `--jit-basic-blocks` selects
the previous behavior for controlled comparisons; `--jit-follow-jumps` remains
an explicit enable. The Jit constructor's Basic default remains for callers
requiring a one-basic-block contract; production callers choose FollowJumps.
Metadata and benchmark reports record the effective mode. Snapshot reports
also retain emulator/task hashes and reject changes during measurement.

Post-change validation passes the full unit suite, normal/profiled DOS smoke
matrices, Win3.11 exact Notepad file saving, both Win95 sources' cold boot and
guest restart, and Word/Excel task pixels under default/basic/interpreter modes.
Both Win95 metadata files confirm joining enabled with no explicit mode flag.
All checks run headless; compatibility timings from concurrent tests are not
performance comparisons. `build/joined-default-validation.json` records source
hashes and evidence locations. This policy change uses the already-verified
joined emitter and does not introduce another translation algorithm.


### Refreshed direct Rosetta comparison (2026-10-24)

A frozen validated emulator binary (matching the default-joining build recorded
in `build/joined-default-validation.json`) repeats the direct arithmetic and
RAM comparisons headless. The built-in isolated kernels deliberately retain
the Basic block contract; these are not measurements of the conditional-trace
experiment or Windows startup. Register runs use 5,000,000 blocks; memory runs
use 10,000,000. Native/Rosetta results are medians of five warmed iterations;
PCEmu results are one long interval per engine/memory mode. All printed checksums
match their expected values, including EAX=240,000,000 and RAM sum=160,000,000.

| Pattern | PCEmu JIT million equivalent instructions/s | Rosetta million equivalent instructions/s | Rosetta / PCEmu |
| --- | ---: | ---: | ---: |
| Register arithmetic | 1320.3 | 4496.9 | 3.41 |
| Flat guest RAM | 2215.0 | 11970.3 | 5.40 |
| Paged guest RAM | 924.1 | 11970.3 | 12.95 |

The memory reference is the same host x86-64 kernel in both rows; it does not
implement guest paging. These gaps therefore include emulator MMU semantics
which Rosetta avoids. As before, the loops have comparable operations, not
identical instruction encodings/termination logic, and omit devices, faults and
application startup. No whole-Windows parity or percentage-complete estimate
follows from them. Raw logs and runner hashes are retained separately under
`build/rosetta-current/`; earlier evidence is preserved.

`tools/bench_rosetta.py --emulator PATH --output PATH` can now use a frozen
executable without rebuilding and retain a new result instead of overwriting
an older one. It checks every PCEmu checksum against the native result, records
the executable hash and rejects a changing executable. All benchmark launches
explicitly request headless mode.


### Conditional native trace experiment (2026-10-24, opt-in)

`--jit-traces` follows predicted conditional paths as well as direct jumps,
retaining GPRs across basic blocks. Backward branches predict taken; forward
branches predict fallthrough. An unexpected outcome exits with exact flags,
GPRs, destination and executed-prefix instruction count. The same one-page and
64-instruction bounds remain; an unsupported predicted target leaves a terminal
branch with its original two destinations. This is separate from default
unconditional jump joining.

Three alternating same-binary pairs run 20,000 CoreMark iterations, with valid
upstream CRCs, matching 7,950,184,496 guest instructions, and return to DOS:

| Pair | Joined seconds | Trace seconds | Joined / trace |
| --- | ---: | ---: | ---: |
| 1 | 30.374847 | 23.919932 | 1.26986 |
| 2 | 29.597460 | 23.901700 | 1.23830 |
| 3 | 29.773277 | 23.513710 | 1.26621 |

The median paired throughput gain is 26.6%. All intervals exceed ten seconds.
Runner hashes, build inputs and every sample remain in
`build/conditional-trace-comparison/`. Native instructions increase from
7,517,819,881 to 7,540,227,450; arena occupancy grows from 758,512 to 1,757,152
bytes (2.32x). The code-size cost is material, so traces remain opt-in and no
Windows application performance gain is claimed.

A separate diagnostic 1,000-iteration profile executes 397,509,732 guest
instructions. Compared with default joining's matching profile, dispatches and
device polls fall from 97,379,695 to 53,907,241 (44.6%), native calls fall from
75,752,046 to 33,400,277, and interpreted attempts from 21,627,649 to 20,506,964.
Average native instructions per call rise from 4.96 to 11.29. Profile timings
are excluded from the comparison.

The full unit suite passes; final 183,478 flag checks cover every Jcc and flag
image, both prediction directions, 16/32-bit execution, terminal successors,
CMP before later flag overwrites, exact memory faults and code-changing stores.
All 28 DOS microkernel intervals pass. Headless compatibility checks verify
Win3.11 Notepad exact contents, rtts/installed OSR2 cold boot and restart, and
Word/Excel task pixels. They do not rerun the installer in trace mode.
`build/conditional-trace-validation.json` records evidence and source hashes.
The normal emulator, DOS benchmark and snapshot task runner accept the mode;
metadata records it separately from unconditional joining.


### Combined TLB keys and block-local epoch (2026-10-24)

The paged RAM hit path now validates the adjacent tag/epoch pair with one
64-bit load and comparison. This replaces two 32-bit checks without changing
TLB storage or guest paging rules. Where a spare callee-saved register exists,
the current epoch is loaded once per native entry and retained across ordinary
RAM helpers. Register allocation prefers already-saved pairs; fully occupied
blocks retain the per-access epoch load. User permissions, first-write dirty
updates, RAM bounds, device checks, code writes and precise replay remain.

A common-source snapshot builds three frozen variants, changing only
`jit_arm64.cpp`: the preceding implementation, combined keys alone, and combined
keys plus epoch retention. Build/source hashes and commands are retained in
`build/packed-tlb-comparison/`. Both comparisons use three alternating pairs of
9.8 billion guest instructions per flat/paged case. Every case checks the sum
3,200,000,000, and native instruction counts match. The built-in kernels retain
their Basic policy; these are MMU-path comparisons, not conditional-trace tests.

| Variant | Median paged million instructions/s | Median paired paged ratio | Median paired flat control ratio |
| --- | ---: | ---: | ---: |
| Combined keys alone | 896.7 (baseline 866.8) | 1.03442 | 1.00442 |
| Keys plus retained epoch | 936.2 (baseline 880.6) | 1.06314 | 0.97663 |

The combined change's paged ratios are 1.02747, 1.06314 and 1.12721: an observed
median gain of 6.3%. The flat code path is unchanged, but its ratios vary with
host conditions, so no flat-RAM improvement is claimed. All samples, including
the slow final pair in the keys-only trial, remain in the evidence. These small
trials do not establish a precise general speedup or close the Rosetta gap.

Five alternating headless pairs per application on the unchanged VGA fixtures
show the following end-to-end response-time upper bounds:

| Task | Baseline median ms | Changed median ms | Median paired ratio |
| --- | ---: | ---: | ---: |
| Word | 1656.8 | 1554.2 | 1.06542 |
| Excel | 751.9 | 734.9 | 1.00136 |

Word's paired ratios range 0.90716–1.12374; Excel's range 0.97316–1.19389. Thus
Word has an observed 6.5% paired median improvement and Excel is effectively
neutral, with substantial noise. Dispatch/poll wall-time paired medians are
1.06614 and 0.99925 respectively. Timing records and runner hashes are under
`build/packed-tlb-comparison/office/`; no universal startup gain is claimed.

`--benchmark` and `--benchmark-memory` now honor explicit
`--backend jit|interpreter` selection, so long native trials need not also run
the slower interpreter. The default auto mode still runs both and reports the
speedup. This selection is identical in all frozen comparison binaries.

New CPU regressions cover collisions at the same epoch, remapping with the
same tag, high epoch bits, wraparound, byte/dword access, spare-register and
fully occupied-register blocks, and preservation of untouched guest GPRs.
The complete suite passes, including 339,746 core checks. Integration evidence
and current source hashes are listed in `build/hoisted-tlb-validation.json`.


### Rejected 32-byte TLB layout experiment (2026-10-24)

A frozen same-source experiment padded and aligned each TLB entry from 20 to
32 bytes, replacing stride multiplication with a shift and aligning the packed
key load. This increases the 1,024-entry table from 20 to 32 KiB. It is not
retained in the implementation.

Three alternating pairs of 9.8-billion-instruction headless memory kernels
produced paged candidate/baseline ratios 1.04818, 1.07360 and 1.11718 (median
+7.4%). Flat ratios were 0.99646, 1.02317 and 1.07101. All checksums and native
instruction counts matched; the candidate passed 339,746 focused core checks.
These results alone do not establish an application benefit.

Five alternating headless pairs per Office task gave median paired response
ratios (baseline/candidate, higher is better) of 0.91894 for Word and 1.00508
for Excel. Dispatch/poll wall-time ratios were 0.99594 and 1.01834 respectively.
Word's observed response slowdown is about 8.1%, but host and capture timing
noise prevents attributing all of it to the layout. Excel is effectively
neutral. With no demonstrated broad benefit, the compact layout remains.

Frozen sources, binary hashes, all kernel trials and application trials are
retained locally under `build/aligned-tlb-comparison/`, including
`result.json` and `office/result.json`. Full cold-boot compatibility validation
was not run for this rejected candidate; the application trials verified all
20 restored tasks against their expected pixels.


### Page-amortized REP STOS (2026-10-24)

Forward repeated fills now reuse one validated ordinary-RAM translation up
to a linear page boundary, address-size wrap, or the existing 4,096-element
scheduling budget. Byte fills use memset; word/dword fills store the repeated
accumulator value. Paged fills require an existing permitted, dirty TLB entry
with matching CR3 and epoch. The scalar path performs page walks and first
writes, retaining precise faults and accessed/dirty-bit ordering. Reverse
fills, disabled A20, write observers, VGA memory and destination code-bitmap
words retain scalar execution. No host virtual-memory mapping is required.

An additional 192 comparisons against forced scalar execution cover paged
and flat RAM, all element widths, both address sizes and directions, page
and address wrapping, scheduling yields, VGA callbacks and code generation.
The core suite passed 339,938 checks and the full make test completed before
the concurrent storage cleanup removed its logs and binaries. Existing REP
fault-progress tests also passed. No new full guest-boot validation or Office
performance result is claimed for this change.

Exploratory isolated fill timings showed substantial improvement, but the
final timing run was interrupted by removal of its binaries. No finalized
speedup is reported. This optimization addresses a known repeated-translation
cost; the host stack profiles alone do not establish its contribution to
application startup. The cleanup also removed the local evidence directories
referenced by earlier experiments in this document; those historical claims
now have recorded summaries rather than retained raw local evidence.


### Page-amortized REP MOVS (2026-10-25)

Forward REP MOVS now copies page-bounded ordinary-RAM spans after validating
both cached translations. It retains scalar execution for page walks, dirty-bit
updates, permissions failures, TLB collisions, watched writes, VGA accesses and
writes to translated-code bitmap words. Physically overlapping ranges also
retain scalar execution: forward MOVS can propagate values, so substituting
memmove would be incorrect. Both linear page ends, both address-size wraps and
the existing 4,096-element scheduling budget bound each span. The existing
STOS path is kept separately.

The differential matrix now checks 1,152 batch results against forced scalar
execution, including virtual aliases of the same physical bytes, direct-mapped
TLB collisions, overlap in both directions, code writes and VGA callbacks.
The core suite passes 340,898 checks, including existing partial-REP fault cases.

Three alternating paired DOS runs at ten million loops use identical guest
programs and frozen runners differing only in CPU execution. Timed cases copy
64 elements repeatedly in real-mode 16-bit and flat protected-mode 32-bit code;
paging is disabled. Paired baseline/candidate medians are:

| Case | Cold JIT | Warm JIT |
| --- | ---: | ---: |
| MOVSW | 8.86x | 8.96x |
| MOVSD | 12.03x | 13.97x |
| STOSW control | 0.780x | 0.804x |
| STOSD control | 1.002x | 1.022x |

The word-fill control is slower and variable despite preserving its source
path. A separate CPU-only control (two million 64-element fills per interval,
three alternating pairs) has word-fill ratios 1.015, 0.990 and 1.032; this
suggests the slowdown is not intrinsic to the isolated fill loop, but does not
explain or dismiss the DOS result. No general Windows speedup or absence of
regressions is claimed. Copy intervals span approximately 12–284 ms; there are
only three pairs, with substantial host variation. All 32 guest interval
checksums, architectural instruction counts and COM hashes match across the
six DOS runs.

An earlier shared MOVS/STOS implementation was not retained after a slower
word-fill control. Its trials remain alongside the separate-path trials in
the evidence. Both timing variants use the same disk code preceding the CHS
geometry fix, keeping that independent correction out of the CPU comparison.
The geometry correction is validated separately against the compacted OSR2
image. These copy measurements do not establish paged-copy or application gains.


Final validation passes the complete suite, including 340,898 core checks,
2,896 snapshot/COW checks, 2,957,189 byte JIT checks, 27,567 stack checks and
183,478 flag-liveness checks. One earlier host-network forwarding assertion
failed; its standalone rerun and both subsequent full-suite runs passed. The
failure log is retained rather than discarded.

The final binary passes headless Win3.11 Notepad save, rtts Win95 cold boot and
restart, installed OSR2 cold boot and restart, DOS CoreMark CRCs, 64 micro
intervals across both engines, wrong-checksum rejection and interval profiling
consistency. This does not repeat the fresh OSR2 installer or Office startup.
Provenance, individual timings, the slower fill controls and runtime evidence
are committed in `docs/measurements/rep-copy-2026-10-25.json`. Larger local
artifacts are under `build/rep-copy-comparison/`; successful private guest disks
are removed.


### Shorter user-mode RAM permission guards (2026-10-25)

Paged user loads now check the user-readable bit directly with LDRB/TBZ, reducing
the permission check from six ARM64 instructions to two. User stores validate
adjacent permission/dirty bytes with LDRH, CMP and a conditional branch, replacing
eight instructions with three. TLB entries remain 20 bytes; no RAM, snapshot or
disk format is expanded. Supervisor-mode emission is unchanged.

A page that passes user permissions is also valid during a system-access
override. Pages that do not pass conservatively use the existing helper, which
applies the current override before deciding whether to fault. Unknown permission
bits may force a store fallback, but cannot bypass protection. First-write dirty
updates and all existing translation, device, code-write and fault guards remain.

The new matrix has 384 assertions over byte/word/dword reads and writes, all
user/read-write permission combinations, clean/dirty entries, system overrides
and unknown permission bits. It checks actual native execution, helper use,
precise faults, unchanged data on denial and PTE dirty state. Relevant checks
pass: 341,282 core, 156,510 read JIT, 2,957,189 byte JIT, 27,567 stack JIT and
183,478 flag-liveness checks. Both retained Win95 images cold-boot headlessly to
an unobscured Explorer desktop on the final binary. Successful small compressed
overlays are removed; no new VM images or checkpoints are created.

Five alternating RAM-only pairs run 9.8 billion guest instructions per case.
Each checksum is 3,200,000,000 and every native count is 9,799,999,657. Median
candidate/baseline throughput ratios are 1.08953 paged and 1.06210 flat. The
paged ratios are 1.07703, 1.11497, 1.08953, 1.49053 and 0.98201. The unchanged
flat control also varies heavily (1.00044–1.37177), so the observed roughly 9%
paged median is not a precise causal or general speedup claim. All outliers are
retained. This is CPL3 RAM work, not whole Windows or Rosetta performance.

All raw timing output, hashes, paired ratios and startup results are committed
in `docs/measurements/user-ram-guards-2026-10-25.json`. The small temporary
baseline executable was deleted after measurement to avoid retaining extra
binaries. The baseline hash matches the preceding validated REP-copy build.


### RAM-only byte-access benchmark (2026-10-25)

`--benchmark-memory-bytes` runs the same 49-instruction load/add/store loop as
`--benchmark-memory`, using AL and adjacent byte cells instead of EAX and dword
cells. Flat and CPL3 paged cases run without constructing a machine or opening
a guest disk. `--backend jit|interpreter` selects an engine; auto runs both.
The dword command and its output remain unchanged.

Byte cells start at 1 through 16, and each cell is checked individually against
its modulo-256 result. This detects missing work even when the loop count is a
multiple of 256. Timing excludes initialization and checksums. CLI checks cover
minimum work, rounding to complete loops and 255/256/257-loop wraparound under
both engines (32 flat/paged cases). At 49,000,049 guest instructions both byte
engines produce checksum 1,176; the dword control produces 16,000,016.

Example: `build/pcemu --headless --benchmark-memory-bytes --backend jit
--benchmark-steps 9800000049` executes 200,000,001 loops with expected checksum
152. This is an isolated tiny-working-set kernel, not Windows startup or DRAM
bandwidth. It requires no new VM images or checkpoints.
