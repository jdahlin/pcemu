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


### Omit impossible byte-boundary crossings (2026-10-25)

A single byte cannot cross a 4 KiB page or a 64-byte code-bitmap word. Native
byte loads now omit three page-boundary instructions; stores omit those and
three bitmap-word-boundary instructions. Translation keys, permissions, dirty
updates, segment checks, A20, RAM bounds, VGA checks and the actual code-bitmap
lookup remain. Wider accesses retain both crossing checks. No state or disk
format changes are required.

Twenty-four new checks exercise native byte reads/writes at bitmap-word ends,
page ends and the final RAM byte, with flat/paged RAM and tracked/untracked code.
They check native execution, helper use, exact register/data results and code
generations. Relevant checks pass: 341,306 core, 156,510 read JIT, 2,957,189 byte
JIT and 183,478 flag-liveness checks. Both retained Win95 images cold-boot to
unobscured Explorer desktops. This does not repeat a fresh installer or Win3.11.

Five alternating pairs compare frozen binaries with the same byte benchmark
and guest work. Byte cases execute 9,800,000,049 guest instructions each, with
checksum 152 and 9,799,999,706 native instructions. Dword controls use shorter
980,000,049-instruction intervals, checksum 320,000,016 and 979,999,706 native
instructions. Each cell is checked individually, and every run passes.

| Throughput ratio, candidate/baseline | Flat | CPL3 paged |
| --- | ---: | ---: |
| Byte kernel median | 1.16134 | 1.09029 |
| Unchanged dword control median | 1.02919 | 1.00506 |

Byte-flat paired ratios range 1.14874–1.17644 and byte-paged ratios
1.08546–1.14705. The observed medians are about +16% and +9%; controls show host
variation and are not a formal statistical adjustment. These are isolated
cache-resident kernels, not whole-Windows or Rosetta speedup claims. All raw
output, hashes, ratios and startup evidence are committed in
`docs/measurements/byte-boundaries-2026-10-25.json`.

Validation stays disk-light: RAM-only timing, existing Win95 images with small
compressed overlays deleted on success, and no new VM images or checkpoints.
The temporary baseline executable was deleted after measurement.


## Native register negation (25 October)

A short DOS CoreMark profile identifies 41,030 interpreted register NEG
instructions. Register F6/F7 /3 now lowers to native subtraction from zero,
sharing the existing subtraction flags and backward flag-liveness analysis.
Byte aliases and operand-size overrides preserve the rest of the destination.
Memory NEG retains the interpreter path. The architectural reference is
NEG in the local Intel SDM, Volume 2B pages 4-165–166.

237,334 flag-liveness checks pass, including exhaustive byte values in every
byte alias, wider signed/carry boundaries, mixed randomized sequences and
precise fault/code-invalidation exits. Both short CoreMark runs execute exactly
3,975,114 guest instructions with canonical CRCs. Interpreted attempts fall
from 226,067 to 186,285; dispatches fall from 982,069 to 904,495. These are
instrumented coverage diagnostics, with overlapping build/validation work,
not a measured speed improvement. All 64 DOS microbenchmark intervals match
between JIT and interpreter and return to DOS.

Win3.11 exact Notepad save/return to DOS and both retained Win95 images'
cold boot/guest restart to unobscured Explorer pass; successful compressed
overlays are removed. This does not repeat a fresh installer or establish
Rosetta-level performance. Full interval counters, provenance and compatibility
results are retained in [the measurement record](measurements/native-neg-2026-10-25.json).


### Controlled NEG host timing

Five alternating pairs of uninstrumented DOS CoreMark runs now measure the
change with 10,000 iterations per run. Both runners use the same current
sources and options; only the baseline decoder comes from before register NEG
admission. All ten runs pass canonical CRCs, upstream minimum-duration
validation and DOS return, with identical COM hash and 3,975,092,643 guest
instructions. No competing runner build or validation was launched during the
timed trials.

Baseline/candidate time ratios are 1.11152, 1.09722, 1.11037, 1.09732 and
1.10736: median paired throughput improves **10.74%**. Median interval times
are 15.0585 s and 13.5945 s. These are five samples on one Apple M4, without a
confidence interval, and cover flat integer CoreMark rather than Windows MMU,
GUI startup or Rosetta equivalence. All raw run results, compiler options,
source/binary hashes and host load metadata are committed in
[the timing record](measurements/native-neg-timing-2026-10-25.json). Successful
compressed disk overlays and the temporary baseline binary/source were removed.


## Native register ALU with memory sources (25 October)

ADD/OR/AND/SUB/XOR register destinations now reuse the guarded native memory
read path at byte/word/dword widths. Effective addresses use the original cached
registers, including destination/base aliases. Only a successful read commits
the result and its original flag operands. TLB misses, permission faults, page
crossings, unusable segments and device reads retain the existing helper/replay
path. Memory destinations and ADC/SBB remain interpreted. Architectural forms
follow the local Intel SDM ADD/SUB and logical-operation instruction references.

The read suite passes 243,295 checks, including all destination aliases, code
and address-size combinations, all Jcc predicates, paging cold/hit paths, faults,
MMIO, boundary crossings and production warmup. 237,430 flag-liveness checks
cover precise later faults and code-invalidation exits, and the core and byte
suites pass 341,306 and 2,957,189 checks. Short DOS CoreMark retains the same
COM hash, CRCs and 3,975,114 guest instructions while interpreted attempts fall
186,285 -> 151,824 and dispatches 904,495 -> 839,456. Interpreted opcode 03
(ADD register from memory) falls 32,400 -> 70 attempts. This is diagnostic
coverage; overlapping build/compatibility work prevents timing claims.

All 64 DOS microbenchmark intervals match JIT/interpreter guest work and
checksums. Win3.11 saves exact Notepad contents and exits to DOS; rtts and
installed OSR2 each cold-boot and guest-restart to unobscured Explorer.
Successful private compressed disks are removed. [The committed measurement
record](measurements/native-memory-alu-2026-10-25.json) retains interval counters,
source hashes and compatibility results. Fresh installation and broad
Rosetta-level performance are not established by these checks.


### Controlled memory-source ALU host timing

Five alternating pairs of uninstrumented DOS CoreMark trials, 10,000 iterations
per run, measure the new register-destination memory-source ALU lowering.
The baseline changes only its decoder's admission condition; sources, emitter,
IR layout and compiler options are shared. All ten runs pass canonical CRCs,
upstream minimum-duration validation and DOS return, with identical COM hash
and 3,975,092,643 guest instructions. No competing runner build or validation
was launched during timed trials.

Baseline/candidate time ratios are 1.08557, 1.07582, 1.09833, 1.10367 and
1.09590: median paired throughput improves **9.59%**. Median intervals are
13.6779 s and 12.5611 s. This is incremental over native NEG, on one Apple M4,
without a confidence interval; gains across separate experiments are not
multiplied. It measures flat integer execution, not Windows MMU, GUI startup
or Rosetta equivalence. [All samples and provenance](measurements/native-memory-alu-timing-2026-10-25.json)
are committed. Successful compressed overlays, the temporary baseline decoder,
binary and debug symbols were removed.


## Rejected inline software-TLB wrapper (25 October)

The Win95 host profiles motivated an inline ordinary-translation wrapper around
CPU::physical. It returned directly on nonpaged translations or permitted,
current, dirty-enough TLB hits, and delegated walks, faults, first writes and
CR3 changes to the unchanged full body. 2,048 new core assertions cover A20,
privilege/system-access overrides, permissions, dirty bits, stale tags/epochs,
CR3 mismatch, CR2 and page-table code invalidation. The candidate passes them.

Five alternating pairs per engine use RAM-only kernels and matched source trees.
The candidate/baseline throughput medians are:

| Engine | Flat memory | CPL3 paged memory |
|---|---:|---:|
| Interpreter | 0.90761x | 1.03876x |
| JIT | 1.00224x | 1.02191x |

All checksums and native instruction counts match. The roughly 9.2% flat
interpreter regression outweighs the roughly 3.9% paged gain, so the wrapper
is **reverted**. Compiler code layout/inlining is a possible cause, not an
established diagnosis. The added correctness checks remain. These are small,
cache-resident RAM kernels on one host, not Windows/Rosetta measurements.
[Every paired sample and build hash](measurements/rejected-inline-tlb-2026-10-25.json)
and [the rejected patch](measurements/rejected-inline-tlb-2026-10-25.patch)
are retained, including the initial pilot separately from the paired summary.
No disk image was opened; the temporary frozen source tree, baseline binary
and debug symbols were removed. Future MMU work must retain flat-memory controls
rather than assuming that inlining improves the whole interpreter.


## Gate idle PS/2 board polling (25 October)

Win95 host profiles put 5–7% of main-thread self samples in the controller's
poll function. The board now asks a conservative inline `pollDue` predicate
before building PIC gating inputs and calling the controller. It skips only
when polling cannot change state or raise an IRQ: mouse packet deadlines,
queued replies/scans/packets, full output buffers and unasserted enabled IRQs
remain observable. The predicate reads live state, so guest I/O, host input,
reset and snapshot restore need no cached-deadline invalidation. Direct port
reads still use the existing controller poll path.

65,564 standalone checks pass, including 32,768 differential state/IRQ cases
against unconditional polling at both sides of deadlines and all PIC gating
combinations. The 343,354 core checks, 2,896 snapshot/COW checks and 106 input/
video integration checks pass. Win3.11 saves exact Notepad text and exits to
DOS; both retained Win95 disks cold-boot and guest-restart to Explorer.

Five alternating matched DOS CoreMark pairs execute 10,000 iterations each,
with identical COM hash, 3,975,092,643 guest instructions and 3,833,382,323 native
instructions. Every run passes canonical CRCs, upstream minimum-duration
validation and DOS return. Baseline/candidate time ratios are 1.03996, 1.08115,
1.05157, 1.04848 and 1.07492: median paired throughput improves **5.16%**.
Median intervals are 12.5721 s and 11.8448 s. Only the board polling gate differs
between runners, and no competing build/validation was launched during timing.
This measures flat integer work with real Machine/device polling on one Apple
M4, without a confidence interval; it does not establish Windows/Rosetta gains.

A fresh rtts boot host profile also reaches unobscured Explorer, with 7,626
main-thread samples after attachment waits are excluded. Device polling is
11.0%, interpreter work 51.6% and dispatch 19.4%. This includes OCR/dialog
observation and is not a matched boot-speed comparison. [All timing samples,
checks, host summary and source hashes](measurements/ps2-poll-2026-10-25.json)
and a [compressed raw call tree](measurements/ps2-poll-win95-2026-10-25.sample.txt.zst)
are committed. Successful compressed overlays and the temporary baseline runner,
source and debug symbols were removed; no new installer or Office fixture was
created.


## Defer redundant paging privilege evaluation (25 October)

CPU::physical retains its out-of-line translation body, but evaluates CPL and
system-access override only when combined PDE/PTE permissions restrict the
requested access, or when encoding a page fault. Pages already permitting user
reads/writes also permit supervisor/system accesses. Restricted pages, missing
entries, dirty-bit updates, CR3/epoch invalidation, A20 and precise CR2/error
reporting retain their previous behavior. This avoids the broadly inlined
wrapper rejected above.

The core suite passes 343,418 checks. This includes the existing hit/miss,
permission, A20, dirty and code-invalidation matrix and 64 new assertions for
nonpresent PDE/PTE fault U/S/W/R encoding across VM86, CPL and system overrides.
Read/byte/flag suites pass 243,295 / 2,957,189 / 237,430 checks. DOS benchmark
CRCs, 64 micro intervals, matching JIT/interpreter work and wrong-checksum
rejection pass; Win3.11 exact Notepad save and both Win95 cold-boot/restart
checks pass headless.

Five alternating RAM-only pairs per engine give these candidate/baseline
throughput medians, with matching checksums/native counts:

| Engine | Flat memory | CPL3 paged memory |
|---|---:|---:|
| Interpreter | 0.98766x | 1.02731x |
| JIT | 1.00084x | 1.01224x |

The flat interpreter's 1.2% regression is a retained tradeoff, not a neutral
control. Five additional alternating rtts Win95 pairs each stop at exactly
300,000,000 guest instructions with 217,575,627 native instructions and 282,224
slow memory calls. All captured state fields match except host timing and
private disk path. Process-wall ratios are 1.02705, 1.03423, 1.02672, 1.02350
and 1.01595: median paired throughput improves **2.67%**. CPU/device-batch wall
ratios have median 1.02965. The gateway is disconnected, no UI automation or
frame capture runs during execution, and this is a bounded startup workload,
not time to Explorer or a byte-for-byte final RAM comparison.

The change is retained for its measured paged/Win95 improvement with the smaller
flat cost documented. These are one-host samples without confidence intervals,
not broad Windows or Rosetta equivalence. [All samples, build/source hashes,
full captured states and compatibility checks](measurements/lazy-page-permissions-2026-10-25.json)
are committed. Each successful boot used a compressed child allocating 397,312
bytes; all were removed. RAM kernels opened no disk. Temporary baseline source,
binary/debug files were removed, without creating installer/Office fixtures.


## Guarded native memory INC/DEC (25 October)

Memory FE/FF /0 and /1 without LOCK now lower at byte, word and dword widths.
The existing writable/dirty TLB, ordinary-RAM, A20, page-boundary, code-bitmap
and watched-write guards remain in place. The inline path loads the original
operand, updates RAM, and records the original value for flag materialization.
Carry is published from preceding pending flags before access and preserved by
INC/DEC. Declined accesses replay with the original IP/flags. The slow path
probes read translation before write translation, preserving the interpreter's
callback/dirty-bit ordering for devices and write faults. LOCK-prefixed forms
retain interpreter fallback. Architectural flag behavior follows INC/DEC in
the local Intel SDM, Volume 2A pages 3-516 and 3-328.

The new `build/jit-rmw-tests` target is included in `make test` and passes
947,820 checks: every byte value, wider boundary values, code/address-size
combinations, all Jcc conditions, preserved carry, warm/cold paging, A20 aliasing,
permission faults, device/watch callbacks, cross-page replay, code generation
changes and later fault/invalidation boundaries before flag overwrites. Core,
read, byte and flag suites pass 343,418 / 243,295 / 2,957,189 / 237,430 checks.
DOS profile validation retains all CRCs and 64 matching microbenchmark intervals.
Win3.11 exact file-save and both Win95 Calculator/clipboard/Notepad save/restart/
exact FAT-readback checks pass headless; successful compressed disks are removed.

At the same 3,975,114 guest instructions in short diagnostic CoreMark, native
instructions rise 3,823,290 -> 3,880,832, interpreted attempts fall
151,824 -> 94,282 and dispatches fall 839,456 -> 781,914. Instrumented timing is
not a benchmark score. Five alternating uninstrumented pairs use the complete
pre-change source baseline, including its former helper and metadata layout.
All ten 15,000-iteration runs pass canonical CRCs, upstream duration validation
and DOS return, with identical COM hash and 5,962,639,001 guest instructions.
Baseline/candidate interval ratios are 1.14206, 1.16631, 1.15740, 1.16503 and
1.13867: median paired throughput improves **15.74%**. Median intervals are
20.9031 s and 18.0480 s. Absolute times vary across pairs; every result and host
load record is retained. This is one-host flat integer CPU/device work without
confidence intervals, not Windows application or Rosetta equivalence.

[The complete evidence](measurements/native-memory-incdec-2026-10-25.json)
retains raw timings, counters, compiler/source/binary hashes and compatibility
results. A [small original x86 fault probe](measurements/rmw-fault-reference-2026-10-25.c)
observes macOS Rosetta exception-state error codes 4 / 6 / 4 for load / store /
INC on a PROT_NONE mapping. This supports the read-fault replay oracle used here,
not a claim of native Intel-hardware conformance. Intel's [page-fault definition](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-system-programming-manual-325384.pdf)
defines W/R from the access causing the fault. No broad page-fault policy was
changed. Temporary baseline binaries, debug symbols and the frozen source tree
were removed; no new installer or Office fixture was created.


## Windows host profiles after memory INC/DEC lowering (25 October)

Fresh headless rtts and installed OSR2 runs both pass cold boot to Explorer,
Calculator 2+2, clipboard transfer to Notepad, a fresh save, guest restart,
and exact 38-byte stopped-guest FAT readback. Both private compressed disks
were removed. These validate installed guests; they do not repeat OSR2 Setup.

The main-thread wall-stack profiles contain 29,592 / 26,729 samples after
excluding attachment barriers. Interpreter including MMU/fault work accounts
for 53.2% / 54.9%, JIT dispatch/cache/validation 18.1% / 16.8%, device polling
10.4% / 10.1%, generated code/helpers 7.8% / 7.5%, and disk/compression
0.3% / 0.3%. CPU::physical self samples are 11.0% in both; these overlap
the categories and must not be added to them. Unknown generated frames and
unwind attribution limit symbol precision.

These runs include observer-driven dialogs, applications, capture and restart.
They are workload profiles, not isolated startup timings, matched before/after
measurements, or proof of a Windows speedup. Each run records its own binary
hash; the binary was rebuilt between runs, so their absolute timings must not
be compared. The agreement supports prioritizing frequent Windows interpreter
fallbacks alongside dispatch/block linking and cheaper software-MMU paths.
High native coverage in CoreMark alone does not establish Windows coverage.

[Complete profiles, binary hashes and verification results](measurements/win95-post-rmw-2026-10-25.json)
include SHA-256 references to the two committed zstd-compressed raw sample
traces. The sample-accounting unit tests pass all four cases. No Office or
installer fixture was created.


## Matched CoreMark workload against QEMU TCG (25 October)

Three alternating pairs compare the identical 11,992-byte, 15,000-iteration
CoreMark COM on the same Apple Silicon host. QEMU 10.2.2 uses one 486-model
CPU, single-thread TCG, 16 MiB RAM, no display or network, and a tiny original
BIOS loader. PCEMU uses its headless minimal-DOS benchmark runner and default
JIT jump joining. Both execute the same flat 32-bit protected-mode workload
with paging disabled; boot/loading is outside the timed interval.

PCEMU intervals are 15.8339 / 15.5941 / 15.6074 seconds, versus QEMU
38.0099 / 38.8826 / 38.3661 seconds. Paired throughput ratios are
**2.4005 / 2.4934 / 2.4582**, median **2.4582x in PCEMU's favor**. These
three pairs support a narrow flat integer workload result on this host.

QEMU receives the unchanged guest's 0xe7 start/stop markers through a Unix
socket; the host timestamps receipt. PCEMU timestamps inside its port handler.
Transport/scheduling error is not calibrated. The long compute intervals help
reduce its relative impact, but these are different timing mechanisms. QEMU
lacks the custom 0xe8 guest clock, so the unchanged guest reports zero guest
ticks and fails upstream duration validation. Canonical seed/list/matrix/state
CRCs and the exact iteration count are independently checked; other reported
guest errors are rejected. This is a matched workload comparison, **not an
official CoreMark score**.

[All paired timings, versions/hashes, host load and guest transcripts](measurements/qemu-coremark-2026-10-25.json)
are retained. The initial QEMU probe is excluded from the paired aggregate.
This result does not establish performance relative to QEMU for Windows,
paging, devices, or other instruction mixes. No confidence interval is claimed.
Successful QEMU disks (a tiny COM plus sparse 1 MiB tail) and PCEMU compressed
DOS children are removed; no installed VM or Office fixture is created.

To reproduce each engine's measurement, alternate these commands with a fresh
output directory for each run:

```sh
python3 tools/bench_guest_perf.py --suite coremark --iterations 15000 --output build/pcemu-comparison-run
python3 tools/bench_qemu_coremark.py --program build/pcemu-comparison-run/program/PCBENCH.COM --iterations 15000 --output build/qemu-comparison-run
```

The original QEMU-only BIOS loader is `tests/guest_perf/qemu_boot.asm`; it loads
the COM from LBA 1 without a DOS installation and the host stops QEMU after
the guest's completion marker. QEMU's [debug console implementation](https://github.com/qemu/qemu/blob/master/hw/char/debugcon.c)
and [character-device documentation](https://www.qemu.org/docs/master/system/qemu-manpage.html)
describe the configurable I/O port and Unix socket backend used here. Seven
validator tests cover canonical checks, wrong/missing CRCs, iteration mismatch,
other guest errors and the specific unavailable-clock diagnostic.


## Native near indirect JMP (25 October)

FF /4 near jumps now lower register and memory targets at 16/32-bit operand
widths. The target is read dynamically on every execution; operand width
selects its truncation independently of code/address size. Register transfers
leave GPRs and flags untouched. Memory transfers reuse guarded RAM reads and
commit IP only after success. Fault/MMIO replay retains the original prefixed
instruction and preceding flags; destination-fetch faults occur on the next
dispatch. A successful helper exit after a code-page generation change also
retains the loaded target. Single-transfer blocks are admitted after warmup.
Indirect CALL and far control transfers remain interpreted.

The new `build/jit-jump-tests` target in `make test` passes 17,861 checks:
all GPRs, all block modes, code/operand/address-size combinations, FS and SS
based/SIB addressing, pending flags, changing pointers without recompilation,
read-only/warm/cold paging, precise faults, device callback counts, cross-page
reads, A20 aliases, deferred destination-fetch faults, preceding code writes,
and page-table accessed-bit writes invalidating the executing code page. Core,
read, memory INC/DEC and flag suites pass 343,418 / 243,295 / 947,820 / 237,430
checks. DOS CRCs, all 64 matching micro intervals and wrong-checksum rejection
pass. Win3.11 exact Notepad save/DOS exit and both Win95 application/restart/
exact FAT file checks pass headless; successful private disks are removed.
This does not repeat the OSR2 installer on this binary.

Five alternating uninstrumented CoreMark pairs compare against the complete
pre-change source at `83d1620`, including its original IR layout. All ten
15,000-iteration runs use the same COM hash and 5,962,639,001 guest instructions,
pass canonical CRCs and upstream duration validation, and return to DOS. Paired
baseline/candidate interval ratios are 1.06571, 1.11445, 1.08081, 1.07523 and
1.08255: **median paired throughput improves 8.08%**. Median intervals are
15.5420 / 14.3568 seconds. Native instructions increase 5,836,732,343 ->
5,905,372,336 (97.89% -> 99.04% coverage); resident native bytes rise
810,592 -> 811,056. These are one-host flat integer workload results without
confidence intervals, not Windows startup or Rosetta equivalence.

[Complete paired evidence, source/binary hashes and guest validation](measurements/native-indirect-jump-2026-10-25.json)
is committed. The temporary baseline source, binary and debug symbols were
removed after success. No installer or Office fixture was created.


## Dependent-load memory sweep (25 October)

`tools/bench_guest_memory.py` builds an original 32-bit x86 pointer-chasing
COM and runs it headless. The exact same COM runs in a CPU/JIT-only RAM
harness, the reference interpreter, or QEMU TCG with a tiny BIOS loader.
This is inspired by the dependent-load method described by
[lmbench](https://lmbench.sourceforge.net/man/lat_mem_rd.8.html), not a port
of its source or a published lmbench result. No installed VM fixture is used.

The guest builds deterministic Fisher-Yates shuffled single-cycle rings of
16 / 256 / 1,024 / 2,048 / 4,096 nodes. Each node holds its successor's address.
With 4-byte spacing, spans are 64 bytes to 16 KiB; with 4,096-byte spacing,
the same node counts touch that many distinct guest pages, spanning up to
16 MiB. Only four bytes per sparse page are loaded, so spans are not amounts
of memory transferred. The exact same unrolled loop executes 32 dependent
`MOV EAX,[EAX]` loads per iteration plus seven final loads, preventing a whole
number of ring laps from trivially returning the initial pointer.

Each case runs with paging disabled and with supervisor 4 KiB identity paging
over 64 MiB RAM. Initialization and a warmup of the exact load loop occur
outside the interval; the timed region includes loop/dispatch/guard costs.
The host independently generates each ring permutation and checks the final
pointer, case order and workload count. Metrics are **host ns per logical
dependent guest load**, not raw host DRAM latency or streaming bandwidth.
The CPU/JIT harness excludes Machine/device polling; QEMU runs its full PC
model. These comparisons do not rank overall emulator or Windows performance.

PCEMU timestamps within port handlers; QEMU timestamps Unix-socket receipt
of the same guest start/stop markers. Transport/scheduling error is not
calibrated. The default 4,000,000 iterations execute 128,000,007 dependent loads
per case; intervals below 0.1 seconds are explicitly flagged. The case order
is fixed and user mode, faults, MMIO, SMC and streaming writes/copies are not
covered. Existing correctness suites remain necessary for those behaviors.

```sh
python3 tools/bench_guest_memory.py --backend jit --output build/memory-jit-run
python3 tools/bench_guest_memory.py --backend qemu --output build/memory-qemu-run
python3 tools/smoke_guest_memory.py --qemu
```

`make smoke-guest-memory` checks JIT versus interpreter without requiring QEMU.
The smoke test verifies identical program hashes, transcripts and JIT/reference
instruction counts, then replaces a dependent load with NOPs and requires
rejection of the corrupted guest's results. Six validator unit tests are
included in `make test`. PCEMU opens no disk; successful QEMU sparse 1 MiB
boot images allocate only the tiny program and are removed after validation.

Three alternating JIT/QEMU pairs use identical program hashes and guest
transcripts, with 128,000,007 dependent loads per case. All 120 intervals
exceed 0.1 seconds and all final pointers match. Median host ns/load:

| Working set | Paging | PCEMU CPU/JIT | QEMU TCG PC |
| --- | --- | ---: | ---: |
| Dense 16 KiB / 4 pages | Off | 2.27 | 2.67 |
| Dense 16 KiB / 4 pages | On | 5.77 | 2.67 |
| One pointer/page, 256 pages | On | 10.36 | 69.45 |
| One pointer/page, 1,024 pages | On | 11.96 | 68.83 |
| One pointer/page, 2,048 pages | On | 20.24 | 72.12 |
| One pointer/page, 4,096 pages | On | 33.01 | 82.25 |

These results expose a weakness that CoreMark coverage hides: the dense paged
case is slower than QEMU here even with zero slow-memory calls. Inline native
translation guards are therefore a useful optimization target. At 1,024 sparse
pages occasional data/code TLB collisions invoke the helper; above the
1,024-entry software TLB capacity nearly every sparse load invokes it. Larger
spans also exercise the host memory hierarchy, so the entire slowdown must not
be attributed to software TLB misses. All measured JIT intervals exceed
99.99998% native instruction coverage, but accesses completed by C++ native
helpers count as native too. Coverage is not host time or helper-free execution.

[Full paired results, raw transcripts, source/binary hashes and smoke evidence](measurements/guest-memory-sweep-2026-10-25.json)
are committed, including all timings and host-load observations. Three pairs
on one host without confidence intervals support these specific harness
results. The earlier one-million-iteration calibration is excluded. Both
QEMU-inclusive smoke and `make smoke-guest-memory` pass exact results, matching
JIT/reference instruction counts, native execution and corrupted-kernel
rejection. No production CPU/JIT behavior changed in this benchmark addition.


## Rejected TLB-base address hoist (25 October)

An experiment reserved an unused callee-saved host GPR for the CPU's fixed
TLB-array address in paged blocks with at least four fast memory accesses.
It preferred an already-saved register pair and retained the original path
under register pressure. This removed two repeated address ADDs per access
without removing translation, permission, fault or RAM/device guards.

Three alternating 128,000,007-load-per-case pairs reject the change. The
16-node dense paged ring's median baseline/candidate cost is 1.7453 /
5.2331 ns/load (median paired throughput 0.33307). The 16-page sparse ring
is 1.7766 / 5.4143 ns/load (0.31700). Dense 256/1,024-node ratios are
0.64435 / 0.63987; larger dense cases are near neutral. The 2,048-page
helper-heavy case improves only 3.02%, and the 4,096-page case regresses
2.89%. Flat emitted code is unchanged; its measured controls vary.

All six sweeps have identical guest results, instruction counts, native counts
and helper counts. Thus the small-ring regression is not explained by extra
interpreter/helper work. The exact microarchitectural cause remains unknown;
these measurements do not establish a dependency or alignment explanation.
Fewer emitted instructions did not produce faster execution on this host.
The production emitter is restored byte-for-byte to its pre-experiment source.

[All paired samples and validation](measurements/rejected-tlb-base-hoist-2026-10-25.json)
and the [rejected patch](measurements/rejected-tlb-base-hoist-2026-10-25.patch)
are retained. The new pressure tests remain: 255 nonempty live-GPR masks,
byte/word/dword reads, mixed stores, warm/cold translations and supervisor/user
contexts compare full GPR/control/memory state against the interpreter.
The read-JIT suite now passes 255,535 checks; restored-emitter JIT/reference/
QEMU smoke and corrupted-guest rejection pass. No production runtime change
is retained and no Windows speedup is claimed.

The memory wrapper now accepts `--runner PATH` for controlled PCEMU baseline
comparisons, skips rebuilding that executable, rejects QEMU/custom-runner
combinations and checks the executable hash before/after measurement. The
temporary baseline executable was removed after validation; tests use RAM and
only the existing tiny successful QEMU boot fixture, removed by the wrapper.

## Native register bit tests (25 October)

BT, BTS, BTR and BTC register destinations now emit AArch64 instructions for
both immediate and register indices, with 16/32-bit operand-size overrides.
The index is captured before an aliased destination changes, and is reduced
modulo the operand width. CF receives the original selected bit; every other
flag is preserved exactly as in the interpreter. Flag liveness tracks this
partial write. Memory bit strings and reserved immediate-group encodings
retain the interpreter path.

Differential tests cover every immediate byte, all register overlaps, negative
and high-bit-set register indices, pending ADD flags, preserved upper GPR
halves and a following carry branch. The flag suite passes 25,862,006 checks;
the core suite passes 352,644. Frozen-candidate headless Win3.11 file save/exit,
rtts and fresh OSR2 Calculator/clipboard/Notepad/restart checks pass. Excel
reopens the recovered workbook with A1's formula and result intact.

Five alternating RAM-only kernel pairs give a median paired throughput ratio
of **6.8838x**. The candidate executes all 40 million guest instructions
natively; baseline/candidate AX, BX, CX, flags and instruction counts match.
Owned build and integration processes had exited before these final pairs;
earlier overlapping calibration runs were excluded. This is a targeted bit-test
kernel result, not an Office or Rosetta comparison. The earlier Excel profile
identified opcode 0F BA among frequent fallbacks, but memory forms remain
interpreted and application impact has not been measured.

[Samples, frozen-source/binary hashes, build commands, kernel and integration evidence](measurements/native-bit-test-2026-10-25.json).

## Excel recalculation after register bit tests (25 October)

The recovered 4,096-row dependent-recalculation checkpoint restores useful
application testing on the new external disk chain. Three alternating pairs
use identical frozen whole-machine sources and headers; baseline substitutes
only the pre-bit-test decoder, emitter and flag-liveness files. Every run starts
from the same checkpoint and must render the exact changed SUM pixels.

The median paired baseline/candidate ratio is **1.0361** for response latency
and **1.0379** for execution-batch wall time. All three pairs favor the candidate;
response ratios range from 1.0193 to 1.0725. Native coverage changes from about
88.37% to 88.98%. This is a modest observed improvement on one loaded Excel
task, with three pairs and no confidence interval. It does not demonstrate a
general application speedup or Rosetta parity. Restore readiness, control and
capture costs remain separately visible in the raw results.

A normal-binary host sample covers 14,353 task samples after excluding 7,652
attachment-barrier samples: interpreter 21.7%, JIT dispatch/cache 19.5%,
capture/video/control 19.2%, other main-loop work 15.8%, device polling 12.6%,
generated code/helpers 10.5%, compilation 0.5%, disk/compression 0.2%. These
are host-time categories from a sampled automated task, including capture
overhead; guest native coverage is not a host-time percentage. An initial
sample without the required barrier was rejected and excluded.

A separate instrumented run records 155,012,297 interpreter attempts. Frequent
fallbacks include FF indirect CALL, register ADC (0x13), register SHRD CL,
variable shifts and LAHF. In the sampled Excel routine, on-disk PE bytes show
two adjacent SHRD instructions followed by unsupported `SHR EDX,CL`. The
current minimum-length rule rejects the resulting two-instruction fragment;
this explains why native lowering alone does not eliminate these SHRD
fallbacks. Nearby LAHF/ADC gaps break additional short sequences. Runtime
ModRM counts corroborate the two SHRD source registers; the code inspection
used installed PE sections, rather than a live post-fault RAM dump. These
findings identify the next experiments; no policy change is claimed here.

All timing/profile guests and inspector sessions are stopped, and their owned
private disk children are removed. The reusable compressed checkpoint is
retained externally.
[Paired runs, host/guest profiles, hashes and hot-code evidence](measurements/excel-bit-test-comparison-2026-10-25.json).

## Admit the hot two-SHRD fragment (25 October)

The profiler-identified Excel sequence contains two adjacent register SHRDs
followed by an unsupported CL shift. The JIT now admits exactly this two-operation
kind of fragment after its usual eight-visit warmup. Other short-fragment
thresholds and instruction lowering remain unchanged.

Three alternating matched-source pairs favor the candidate on the restored
Excel task: median paired response ratio **1.0446**, execution-batch wall-time
ratio **1.0457**. Response ratios range from 1.0418 to 1.0535. Native coverage
rises from approximately 88.97% to 90.16%. These are modest observed gains on
one workload with three pairs and no confidence interval, not general emulator
performance or Rosetta parity. No compiler or integration guest ran alongside
the timing pairs.

A separate instrumented run confirms the mechanism: interpreted SHRD CL
attempts fall from **16,764,960 to 54**; total interpreted attempts fall from
155,012,297 to 138,198,971. The remaining warmup/other-site attempts are not
claimed eliminated. Diagnostic timings are excluded from the paired scores.
The reusable checkpoint's files still match their original hashes.

The normal-warmup regression covers both code sizes, both operand sizes, all
256 CL byte values and exact GPR/flags/IP/instruction state against the
interpreter, including zero and oversized counts. The flag suite passes
25,952,090 checks and the core suite 352,644. Frozen-candidate headless Win3.11
Notepad save/exit and both Win95 Calculator/clipboard/Notepad/restart checks
pass; successful private children are removed. A delayed Welcome popup in the
first OSR2 attempt was addressed separately in the unattended checker.

[Pairs, hashes, builds, guest counters and integration evidence](measurements/excel-shrd-pair-2026-10-25.json).

## Native LAHF joins the Excel flag sequence (25 October)

LAHF now materializes pending low arithmetic flags and copies the flags byte
into AH, preserving AL, EAX's upper half and all flags. Flag liveness treats
LAHF as a consumer: a later flag-writing instruction must not make the
flags captured in AH disappear. This also joins Excel's preceding XOR with
the longer native sequence, avoiding its earlier one-instruction fallback.

Three alternating matched-source pairs favor the candidate: median paired
response ratio **1.0488**, execution-batch wall-time ratio **1.0493**. Response
ratios span 1.0332 to 1.0722. Native coverage rises from about 90.17% to 91.37%.
This is a modest observed improvement on the loaded dependent-recalculation
task, with three pairs and no confidence interval; it is not a general
application or Rosetta result. Builds and integration guests ran after timing.

Separate instrumented counts show LAHF falling from roughly 8.4 million
interpreter attempts to **46**, with total interpreted attempts falling from
138,198,971 to 121,432,549. The adjacent XOR also becomes native. These
diagnostic timings are excluded from the score. Saved checkpoint hashes
remain unchanged and all successful private children are removed.

The flag suite passes **37,491,546** checks, including exhaustive byte ADD
inputs/aliases feeding LAHF before a flag-overwriting XOR, both code sizes,
exact AH/other-GPR preservation and all externally supplied low flag bytes.
The core suite passes 352,644 checks. The candidate boots MS-DOS 6.22 to its
ready marker; headless Win3.11 save/exit and both Win95 Calculator/clipboard/
Notepad save/restart checks pass.

[Pairs, source/binary hashes, counters and integration evidence](measurements/excel-lahf-2026-10-25.json).

## Native carry arithmetic in Excel (25 October)

ADC/SBB register and immediate forms now run natively at byte, word and dword
widths, including legacy high-byte aliases and sign-extended immediates. Memory
forms remain interpreted. Incoming CF is captured before destination updates
and retained with the original operands when deferred flag calculation needs
it. A full-width intermediate preserves 32-bit carry/borrow; overflow and AF
use the original operands and truncated result. Flag liveness demands incoming
CF even when the instruction's output flags are overwritten later.

Three alternating matched-source pairs favor the candidate: median paired
response ratio **1.1203**, execution-batch wall-time ratio **1.1171**. Response
ratios range from 1.0520 to 1.1204. Native coverage rises from about 91.36% to
94.07%. This is an observed improvement on one loaded Excel task with three
pairs and no confidence interval. All samples, including the slower third
candidate sample, are retained. It is not a general application/Rosetta claim.
Builds and integration guests ran after the timing pairs.

Separate instrumented counts confirm the coverage change. Register ADC opcode
0x13 attempts fall from **16,785,874 to 391**; its memory forms still fall back.
Register SBB 0x1B attempts fall from 1,701,246 to 7,956. Newly joined native
sequences also remove many neighboring ADD and Jcc fallbacks. Total interpreted
attempts fall from 121,432,549 to **83,569,194**. Warmup, remaining short
fragments and memory forms have not been claimed eliminated. Instrumented
latencies are diagnostic and excluded from the scores.

The flag suite passes **82,250,706** checks, including exhaustive byte operand
pairs with both carry inputs, every register alias, wider carry/overflow edges,
both register directions, immediate encodings, pending/dead-output carry
chains and trace side exits. Exact flags/GPR/IP/instruction state matches the
interpreter. The core suite passes 352,644 checks. DOS reaches its version/ready
markers, and headless Win3.11 save/exit plus both Win95 Calculator/clipboard/
Notepad/restart checks pass. Successful private disks are removed and the
reused checkpoint hashes remain unchanged.

[Paired timings, hashes, counters and integration evidence](measurements/excel-carry-2026-10-25.json).

## Warmed flag-producing ALU/branch pairs (25 October)

The two-instruction admission rule now accepts any supported ALU followed by
a conditional branch after normal warmup. It previously admitted CMP/INC/DEC
but rejected hot SUB/JNA and OR/JS pairs despite supported native lowering.
Other short-block admission rules and instruction lowering remain unchanged.

Three alternating pairs favor the candidate: median paired response ratio
**1.0748**, execution-batch wall-time ratio **1.0763**. Response ratios span
1.0631 to 1.1944; the first pair is noisier and remains included. Native
coverage rises from approximately 94.07% to 96.49%. These are observed results
on one loaded Excel task with three pairs and no confidence interval, not
general application performance or Rosetta parity. Builds and integration
guests ran after the timing pairs.

The initial attempt hit the unchanged baseline's 30-second observation limit
while Excel still showed 75% calculation progress. Its final metadata records
5.23 seconds of execution-batch time; the latency cause is not established.
That process was absent when inspected. All initial successful samples and
the incomplete run diagnostics are retained separately. The complete three-pair
repeat uses a uniform 120-second observation window for both executables,
with identical task commands and result pixels. No completion criterion was
weakened or failed result treated as a successful timing.

A separate instrumented run reduces interpreted attempts from 83,569,194 to
**49,449,740**. SUB/JNA and OR/JS now run as native pairs. Memory-indirect CALL
and CL shifts dominate the remaining fallback counts. Diagnostic timings are
excluded from the score.

Normal-warmup tests exercise all 16 Jcc conditions, all eight ALU groups, both
code sizes and byte/word/dword operands, comparing full GPR/flags/IP/count
state against the interpreter. The flag suite passes **82,496,466** checks,
the core suite 352,644. DOS reaches its ready marker, and headless Win3.11
save/exit and both Win95 Calculator/clipboard/Notepad/restart checks pass.
Successful private disks are removed and checkpoint hashes remain unchanged.

[All samples, timeout diagnostics, counters and integration evidence](measurements/excel-flag-pair-2026-10-25.json).

## Native register shifts by CL (25 October)

SHL/SAL, SHR and SAR register forms now lower natively at byte, word and dword
widths. CL is masked to five bits and captured before aliased destinations
change. Zero counts preserve the destination and flags. Overshifts, CF, AF
preservation and undefined multi-bit OF match the reference interpreter.
Rotates and memory forms remain interpreted.

Flag calculation is skipped when output flags are dead. A dword shift then
maps directly to one ARM variable-shift instruction: its hardware count mask
already matches x86, including a CX/count/destination overlap. Other widths
retain explicit extraction and partial-register insertion. An AF-only liveness
regression verifies preservation of earlier flags through the optimized path.

Three final alternating pairs favor the candidate: median paired response ratio
**1.0672**, execution-batch wall-time ratio **1.0651**. Response ratios span
1.0242 to 1.0769. Native coverage rises from about 96.49% to 97.10%. An earlier
three-pair prototype without the one-instruction path observed a 1.0452 response
ratio; those samples remain separate because its code differs. No own compiler
or integration guest ran alongside the final timing pairs. These are observed
results on one task with three pairs and no confidence interval, not general
application performance or Rosetta parity.

A separate instrumented run reduces register D3 attempts from **8,405,817 to
3,066**; remaining attempts include rotates, short fragments and other sites.
Memory D3 forms remain interpreted. Total interpreted attempts fall from
49,449,740 to **41,016,136**. Diagnostic latencies are excluded from scores.

The flag suite passes **86,490,066** checks, including every CL byte, all
register aliases, both code sizes and operand overrides, live/dead output
flags, zero counts, overshifts and AF-only dependencies. Full GPR/flags/IP/count
state matches the interpreter. The core suite passes 352,644 checks. DOS boots
to its ready marker; headless Win3.11 save/exit and both Win95 Calculator/
clipboard/Notepad/restart checks pass. Successful private disks are removed
and the reusable checkpoint hashes remain unchanged.

[Final/prototype timings, hashes, counters and system checks](measurements/excel-cl-shift-2026-10-25.json).

## Rejected SHRD dead-flag experiment (25 October)

An experiment propagated SHRD flag liveness through runtime-zero counts and
removed its flag calculation when a later instruction overwrote every output.
Known nonzero immediate counts killed incoming arithmetic-flag dependencies;
runtime counts retained them. The flag-free path computed only the shifted
register value, including masked zeros and 16-bit oversized counts.

Six alternating pairs, balanced across both execution orders, do not establish
a useful Excel gain. One pair favors the candidate by 14.9%; the other five
favor the baseline. Combined median response ratio is **0.9953**, execution-
batch wall-time ratio **0.9953**. The second three-pair confirmation has
response ratios 0.9927 to 0.9979. Every sample remains included. Timing
variation and one slower initial baseline are visible; the cause is unknown.
Fewer emitted instructions alone are insufficient evidence for retaining it.

Production emitter and liveness sources are restored byte-for-byte to the
verified CL-shift baseline. New tests remain: all SHRD aliases, count bytes,
code/operand sizes and live/overwritten flags, plus exact flags/GPR/IP/count
state at a faulting FS load before a later overwrite. The restored flag suite
passes **91,745,234** checks and the core suite 352,644. Both candidate and
baseline completed all twelve exact-pixel application runs; successful private
disks were removed and checkpoint hashes remain unchanged. No runtime speedup
or new native coverage is claimed.

[All samples, source hashes and decision](measurements/rejected-shrd-dead-flags-2026-10-25.json),
[rejected patch](measurements/rejected-shrd-dead-flags-2026-10-25.patch).

## CPU accounting for the diskless dependent-load harness (26 October)

`tools/memory_chase_bench.cpp` reports `hostCpuSeconds` alongside the existing
`hostSeconds` wall interval. The process CPU clock surrounds the same execution
loop, including cold compilation and dispatch but excluding setup and result
verification. It excludes time descheduled or blocked; it is not isolated
memory latency and does not remove shared-cache, frequency or contention effects.
The benchmark rejects unavailable/wrapped CPU clocks instead of inventing a
measurement. Very short CPU intervals may round to zero at clock resolution;
use sufficiently long workloads for comparisons. Existing wall-based consumers
remain compatible. Sixteen JIT/interpreter, flat/paged cases pass exact guest
results and positive CPU/wall intervals at one million/20,000 iterations.

### Word 97 document loading: hot short-fragment coverage (2026-10-26)

Supported one/two-instruction fragments that fail the initial cost admission can
now enter native execution after 64 further unchanged executions. Unsupported
instructions retain negative caching. This reduces Word's interpreted attempts
from 12.54 to 9.02 million and raises native instruction share from 87.3% to 90.8%.
Eight rotating holdout rounds show 7.4% lower median task CPU than f4abcad and
11.2% lower than the rebuilt original 57ce863 baseline, with direct linking off.
New tests exercise promotion under interrupts, code changes, RAM replacement
and precise faults. Full scope, controls, limits and reproduction commands are
in [the Word coverage report](measurements/word5m-hot-short-2026-10-26.md).

### Word 97: immediate stores and memory ALU (2026-10-26)

C6/C7 immediate stores and the binary memory ALU family (ADD/OR/ADC/SBB/AND/SUB/
XOR, including carry memory reads) now use the guarded native RAM path. The RMW
helper captures its input before write translation, avoiding a dirty-bit update
changing an operand that aliases its own PTE; regressions include INC/DEC too.
Eight corrected-build holdout rounds show native share 90.8% -> 92.7%, median
Word task CPU 8.8% below 63a3151 and 17.5% below the original 57ce863 baseline.
All 32 restores pass the original document/pixel gate. Correctness tests cover
carry, precise faults, MMIO/watch ordering, self-modification, PTE aliasing,
trace/direct-link paths and interrupt deadlines. See
[the corrected memory-coverage report](measurements/word5m-memory-alu-2026-10-26.md).

### Word and Excel: native memory XCHG (2026-10-26)

Byte/word/dword memory XCHG now uses the guarded RAM path, committing its register
result only after the store succeeds and retaining exact replay on faults/devices.
Separate profiles reduce interpreted word/dword XCHG attempts from 140,592 to 6,191
in Word and from 486,894 to 4,094 in Excel. Native shares reach 92.85% and 99.33%.
All 64 headless restores pass in eight rotating rounds per workload. Candidate
median CPU is 17.4% below the original Word baseline and 7.4% below the original
Excel baseline; the incremental Word timing difference is too small relative to
control variation to establish a reliable gain. See the
[coverage, correctness and measurement report](measurements/office-memory-xchg-2026-10-26.md).

### Word and Excel: native memory bit strings (2026-10-27)

Memory BT/BTS/BTR/BTC now lower to the guarded RAM path, including signed
register bit-string indexes, immediate indexes, precise CF updates and 16-bit
address wrapping before the word displacement. Interpreter comparisons cover
faults, MMIO/watch ordering, self-PTE aliases, code changes and linked loops.
Word's interpreted BT attempts fall from 307,277 to 1,009; Excel's from 80,316
to 397. Native shares reach 93.25% and 99.34%. All 48 unprofiled restores pass;
median CPU is 18.7% below the original Word baseline and 8.3% below the original
Excel baseline, with modest incremental reductions of 1.9% and 1.6% versus the
XCHG build. See the [full results and limits](measurements/office-memory-bits-2026-10-27.md).

### Word and Excel: native LEAVE (2026-10-27)

LEAVE now uses the guarded stack-read path with independent stack and operand
widths, exact fault/MMIO replay, and SS.B cache validation. Interpreted LEAVE
attempts fall from 214,492 to 27,626 in Word and 160,511 to 14,738 in Excel;
native shares reach 93.45% and 99.36%. All 48 headless restores pass. This is a
coverage improvement: incremental timing versus the preceding bit-string build
is neutral within measurement noise. The complete candidate remains 19.9% below
the original Word baseline and 8.6% below the original Excel baseline in the
fresh comparison. See [measurements and correctness scope](measurements/office-leave-2026-10-27.md).

### Word and Excel: register rotates and sign extension (2026-10-27)

ROL/ROR/RCL/RCR now lower natively for byte/word/dword register operands and
implicit-one, immediate and CL counts, including high bytes, zero counts and
the 33-bit carry ring. CBW/CWDE/CWD/CDQ also lower natively. RAM-only exhaustive
byte comparisons and wider boundary cases bring cached validation to 9,425,162
checks. Interpreted attempts fall to 6.08 million in Word and 8.39 million in
Excel; native shares reach 93.88% and 99.41%. All 48 unprofiled restores pass.
The complete candidate uses 20.9% less CPU than the original Word baseline and
8.0% less than the original Excel baseline in the fresh matched comparison.
Incremental median reductions versus LEAVE are 1.5% and 2.5%, with small effects
and host-variation limits documented in the [full report](measurements/office-register-rotate-2026-10-27.md).

### Word and Excel: NOT and memory IMUL (2026-10-27)

NOT now lowers natively for all register widths/high bytes and guarded memory.
Memory IMUL covers 0F AF and 69/6B, with shared signed overflow logic and a saved
64-bit product for deferred flags. Cached validation passes 9,469,038 checks,
including faults, self-PTE/code writes, MMIO and linked flag consumers. Word's
interpreted memory IMUL operands fall from 189,473 to 743; NOT groups also fall
substantially in both applications. Native shares reach 94.12% and 99.43%.
All 60 final restores pass. The complete candidate is 21.7% below the original
Word baseline and 9.7% below the original Excel baseline; incremental timing is
about 1.3% lower for Word and neutral for Excel. Eager/deferred flag strategies
are indistinguishable within timing noise. See the [full comparison and limits](measurements/office-not-imul-2026-10-27.md).

### Word and Excel: guarded integer division (2026-10-27)

DIV/IDIV now lower natively for register operands and side-effect-free RAM hits,
with byte/word/dword widths, aliases, quotient checks and exact interpreter error
replay outside native frames. Cold/unsafe memory replays before translation or
MMIO. Cached validation passes 16,066,216 checks. Interpreted word/dword IDIV
attempts fall from 41,376 to 919 in Word and 33,830 to 361 in Excel; unsigned DIV
also falls substantially. Native shares reach 94.30% and 99.428%. All 48 task
restores pass. The complete candidate remains 20.1% below the original Word CPU
baseline and 9.0% below Excel's. Incremental medians are higher in this run, but
large A/A variation and a separate active VM make attribution inconclusive.
See [the full results and noise limits](measurements/office-divide-2026-10-27.md).

### Word and Excel: memory shifts and rotates (2026-10-27)

Guarded RAM SHL/SAL/SHR/SAR/ROL/ROR/RCL/RCR now share the register count/result/
flag generator. Zero counts still validate writes; cold, watched, device and
code-write paths replay before access. Cached validation passes 20,421,602 checks.
Word's interpreted C1 memory operands fall from 118,272 to 119 and D3 operands
from 13,014 to 56; Excel C1 falls from 13,178 to 40. Native shares reach 94.44%
and 99.429%. All 48 restores pass. Word is 1.07% below the previous division
build, winning all eight paired rounds; Excel's incremental difference is neutral.
The full candidate is 22.9% below the original Word CPU baseline and 9.1% below
Excel's in the fresh matched run. See [results, guard semantics and host limits](measurements/office-memory-shift-2026-10-27.md).

### Word and Excel: sampled interpreter CPU cost (2026-10-27)

Profiling builds now sample thread CPU cost by final decoded opcode, including
faults, with calibrated clock overhead and independent jitter. Fifteen timing
contract checks, six analyzer tests and six real workload restores pass. Release
binaries are byte-identical, so the previous original-baseline results remain the
performance reference. PUSHA/POPA account for an estimated 3.6% of Word and 8.4%
of Excel interpreter CPU; every sampled ARPL faults. Five arena resets in the
mapped Word task also warrant a cache-retention experiment. See the
[method, evidence and limits](measurements/office-interpreter-cost-2026-10-27.md).

### Word and Excel: PUSHA/POPA coverage (2026-10-27)

PUSHA/POPA now execute natively on fully guarded RAM spans, with independent
operand/stack widths and pre-access replay for wrapping, faults, devices and code
writes. New stack-many validation passes 4,724 checks; existing stack, bounded and
cached suites also pass. Interpreted attempts fall about 94% in Word and 97% in
Excel. All 48 matched release restores pass. The candidate is 23.84% below the
original Word CPU baseline and 9.71% below Excel's. Incremental Word timing is
inconclusive; Excel wins all four pairs with a small improvement. See
[coverage, semantics, timing and limits](measurements/office-stack-many-2026-10-27.md).

### Word and Excel: segment MOV coverage (2026-10-27)

MOV from segment registers now lowers for register/memory destinations; MOV to
ES/SS/DS/FS/GS uses guarded selector reads and a warm-descriptor helper shared
with reference descriptor decoding. MOV SS preserves its accounted interrupt
shadow and stays outside cached tails without adding an ordinary hot-hit guard.
Segment validation passes 23,162 checks, plus the reference and batch suites.
Interpreted segment loads fall 85% in Word and 90% in Excel; segment reads fall
88% and 95%. All 60 final matched restores pass. Word CPU is 24.28% below the
original baseline and Excel 10.39%; incremental medians are 1.12% and 0.89% lower.
An initial Excel regression prompted Instruments analysis and removal of a
per-dispatch guard. See [semantics, all runs and limits](measurements/office-segment-2026-10-27.md).

### Word and Excel: PUSHF/POPF coverage (2026-10-27)

PUSHF/POPF now lower with independent operand/stack widths, guarded access,
CPL/IOPL filtering and reserved-flag preservation. POPF returns before the next
instruction for device polling; its boundary blocks stay outside cached tails.
Validation passes 33,512 new checks plus the reference, segment and batch suites.
Interpreted attempts fall 93% in Word and 99% in Excel. All 48 matched release
restores pass. CPU time is 25.27% below the original Word baseline and 10.74%
below Excel's. Incremental medians are 0.87% and 0.38% lower, with host noise
limiting the small differences. See [semantics, measurements and limits](measurements/office-flag-stack-2026-10-27.md).

### Word and Excel: segment PUSH/POP coverage (2026-10-27)

Segment stack operations now lower with guarded stack access, dynamic selector
values and shared descriptor loading. POP SS advances SP using the old SS.B and
retains an accounted interrupt shadow. Validation passes 73,616 new checks plus
the reference, segment, flag-stack and batch suites. Interpreted segment-stack
attempts fall 83.8% in Word and 88.7% in Excel; all 48 matched release restores
pass. Word CPU is 26.04% below the original baseline and Excel 11.18%; incremental
medians are 1.66% and 0.66% lower. See [semantics, measurements and limits](measurements/office-segment-stack-2026-10-27.md).

### Word and Excel: CLI/STI coverage (2026-10-27)

CLI/STI now lower directly with runtime CPL/IOPL guards, precise privilege-fault
replay and full-width accounted STI shadows. CLI can continue into native/linked
blocks; STI uses the existing shadow boundary without adding a cached hot-hit
guard. New validation passes 11,158 checks, including actual pending PIC delivery,
plus the core and batch suites. Interpreted attempts fall 93% in Word and 99% in
Excel. All 48 matched release restores pass. CPU time is 27.26% below the original
Word baseline and 12.22% below Excel's; incremental medians are 0.96% and 1.61%
lower. Word's small difference is mixed; Excel wins all four pairs. See
[semantics, measurements and limits](measurements/office-interrupt-control-2026-10-27.md).
