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
