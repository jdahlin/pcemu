# CPU performance measured inside a DOS guest

```sh
python3 tools/bench_guest_perf.py --output build/coremark-jit
python3 tools/bench_guest_perf.py --no-jit --output build/coremark-interpreter
# Compare against basic blocks; the default JIT joins same-page jumps:
python3 tools/bench_guest_perf.py --jit-basic-blocks --output build/coremark-basic
# Experimental conditional traces (larger code footprint):
python3 tools/bench_guest_perf.py --jit-traces --output build/coremark-traces
# Fast checksum/replay diagnostics, not reportable CoreMark scores:
python3 tools/bench_guest_perf.py --iterations 10 --output build/coremark-quick
```

Each command creates a private **compressed COW disk** against `media/dos.raw`,
replaces CONFIG/AUTOEXEC only in that child, and boots actual MS-DOS. No raw
2 GiB disk copy, mount, Windows startup, screenshot or OCR is involved. A passing
run removes its private disk; a failed run keeps it and its log for inspection.
Results, program binary, object files and exact build options remain under the
requested output directory. The directory must not contain an existing disk.

The original COM program is about 12 KiB with CoreMark or 14 KiB with the
microkernels. Its small loader disables interrupts, masks the PIC, enables A20,
loads a small GDT and enters flat 32-bit protected mode. It copies the payload
to 1 MiB and uses a stack below 2 MiB. The private DOS configuration loads no
HIMEM/EMM386/TSRs. Afterward it restores CR0, the original GDT, real-mode segment
registers, stack, PIC masks and FLAGS, then terminates through DOS INT 21h.
The runner waits for a subsequent DOS batch echo, rather than trusting a marker
emitted before the protected-mode program exits. A20 remains enabled.

The workload is the **unmodified CoreMark source** pinned in
`tests/vendor/coremark/UPSTREAM.json`, currently EEMBC commit
`1f483d5b8316753a742cbf5590caf5bd0a4e4777`. Build verifies every pinned file's
SHA-256. Its upstream README and license notices are retained. This reuse is
benchmark code only; the CPU, devices, loader and runner are original.

Clang cross-compiles for **i386**, using soft float, no SSE/MMX, no host libc,
static 2000-byte benchmark data, and no profile-guided optimization. The linker
and NASM produce a small flat payload/COM image. The port provides volatile
seeds, integer timing, output formatting and minimal memory routines. Full
compiler version, options, upstream revision and COM hash are saved in build.json
and embedded in the result. Dependencies are Clang, NASM and an ELF-capable
`ld.lld` (Homebrew LLVM 17 is detected as a fallback).

## Timing, validation and interpretation

`build/guest-bench` uses the real Machine, CPU, JIT and device polling. It exposes
private test ports only in this runner:

| Port | Operation |
|---|---|
| E4 IN dword | Microbenchmark loop count from the runner |
| E5 OUT dword | Completed microbenchmark checksum; a wrong value fails the run |
| E6 OUT byte | Select benchmark case ID; 1 means CoreMark |
| E7 OUT byte, 1/2 | Begin/end a synchronous host monotonic wall-clock interval |
| E7 OUT byte, 3 | Payload finished; still wait for the DOS return marker |
| E7 OUT byte, 4 | Request a JIT clear after the current dispatch completes |
| E8 IN dword | Monotonic host milliseconds for CoreMark's platform timer |
| E9 OUT byte | Diagnostic transcript, outside the timed algorithm |

The COM image requires this runner; it is not a standalone hardware benchmark
using the BIOS clock. Only the platform timer adapter uses the host clock. The
algorithm is guest x86 code, and both timer reads are outside its inner loop.
Reported nanosecond-resolution intervals include timer-port and start/stop call
overhead; no host process/pipe/screenshot polling chooses their boundaries.
Guest instructions, native instructions, helper calls and compiled blocks are
reported separately. Instruction count is not the primary performance metric.

The runner requires the canonical 2K performance CRCs: list **e714**, matrix
**1fd7**, state **8e3a**. Any algorithm error fails the run. Fixed short runs may
print CoreMark's minimum-duration error; they are explicitly labelled diagnostics
with `upstreamValidated=false`, even when algorithm CRCs are correct.

Default iterations=0 uses upstream automatic calibration and its >=10-second
validation rule. The final measured interval is the performance run; preceding
intervals are calibration. They share one JIT cache, so the final automatic run
is **warmed**, not a cold-JIT score. A fixed-iteration first interval includes
cold guest-code translation (DOS boot code has already run). Never compare the
calibration intervals as performance scores. Each JSON records whether the
upstream validation passed and whether the final host interval was >=10 seconds.
Iterations/host-second is calculated using the precise final host interval.
This is not a certified or submitted result.

The CoreMark configuration isolates 32-bit integer execution, stack/calls/branches and
flat RAM. Paging is disabled. It does not establish Windows GUI, MMU, disk,
network or floating-point performance, or Rosetta equivalence. Keep a separate
16-bit/memory/control-flow microbenchmark matrix and Windows integration tests.
For regression comparisons use the same COM hash and fixed iteration count,
alternate binaries, retain every sample, and report cold versus warm explicitly.


## Initial validation evidence

`build/guest-perf-jit-validated/result.json` completed upstream automatic
calibration and a 13.993-second final interval (3000 iterations, 214.399
iterations/host-second). The interpreter completed a 16.865-second final interval
(2000 iterations, 118.591 iterations/host-second) in
`build/guest-perf-ref-validated/result.json`. Both reported upstream validation,
all three canonical CRCs and successful return to real-mode DOS. These are single
validation runs with different automatically selected iteration counts, not a
statistical before/after claim. Fixed-count diagnostic runs also pass in both
engines. Their shorter times are deliberately not labelled valid scores.


## Original 16/32-bit microbenchmark matrix

```sh
python3 tools/bench_guest_perf.py --suite micro --output build/micro-jit
python3 tools/bench_guest_perf.py --suite micro --no-jit --output build/micro-ref
# Odd-count diagnostic exercises parity and the minimum REP copy count:
python3 tools/bench_guest_perf.py --suite micro --loops 17 --output build/micro-odd
make smoke-guest-perf
```

There are seven kernels in real mode with 16-bit operands, then the corresponding
seven kernels in flat 32-bit protected mode. Both use the original DOS COM loader
and return through DOS. The real-mode version uses 386 32-bit outer counters;
it is not an 8086-compatible benchmark. `--loops` defaults to one million and
accepts 1..10 million without rebuilding the COM image.

| IDs, 16/32-bit | Kernel | Work reported per pass |
|---|---|---|
| 16 / 32 | Sixteen register additions per iteration | 16 × loops additions |
| 17 / 33 | Eight load/add/store updates to one cell | 8 × loops updates |
| 18 / 34 | Alternating odd/even conditional branch | loops iterations |
| 19 / 35 | Four genuine near CALL/RET pairs | 4 × loops calls |
| 20 / 36 | REP MOVSW/MOVSD of 64 elements | max(1, loops/64) × 128/256 bytes |
| 21 / 37 | One addition and a backward LOOP using ECX | loops iterations |
| 22 / 38 | Two AL/AH exchanges with intervening word addition | 2 × loops exchanges |

Each kernel runs twice at the same guest addresses. The first pass explicitly
clears the JIT after the current dispatch returns; the second retains the code
cache. This is **cold/warm JIT**, not cold host CPU caches or cold disk. Both
passes reset their input/accumulator state; the REP destination is zeroed before
each pass. Interrupts are disabled and the PIC is masked in both modes. Boot,
mode transitions, setup before BEGIN, result checking and the return to DOS are
outside the measured intervals. Fixed bookkeeping around the loop remains timed.

Every pass reports a checksum through E5. The host calculates the expected value
independently from the loop count, operand width and data pattern. Missing or
wrong results fail the run. The runner requires all twenty-eight intervals, the
expected cold/warm ordering, and identical guest instruction counts for each
pair. JSON reports work units/host-second, mode, checksum, JIT/native counts and
translation counts. Do not average different units into an overall score.

These deliberately small working sets isolate translation/dispatch overhead;
the single-cell update is not a DRAM-bandwidth test, the copy working set fits
in cache, and the branch pattern is predictable. Paging is disabled. Large/random
working sets, indirect control flow, MMU workloads and Windows GDI remain separate
benchmark work. No FPU instructions are used.

`make smoke-guest-perf` runs short CoreMark and odd-count microkernels under both
JIT and interpreter, compares checksums **and instruction counts** across engines,
checks that short CoreMark runs are not presented as validated scores, and boots
an intentionally wrong guest probe to verify checksum rejection. It requires the
local DOS media and is separate from the media-independent unit tests.

Initial one-million-loop evidence is under `build/guest-micro-{jit,ref}-final/`.
All twenty checksums, mode returns and instruction counts match. The initial
32-bit warm CALL/RET sample takes about 303 ms with JIT versus 272 ms with the
interpreter and reports **zero native instructions**. In contrast, arithmetic
and single-cell updates execute mostly natively. These are individual diagnostic
samples, not a statistical performance comparison, but they identify a concrete
remaining JIT dispatch/control-flow bottleneck without GUI startup noise.

Direct near CALL lowering now reuses the guarded native PUSH path and terminates
its block at the call target. Mixed operand/stack widths, page-crossing replay,
stack faults, MMIO and writes into translated code have differential regression
coverage. At that stage RET remained interpreted. A subsequent diagnostic run at
`build/guest-micro-call-native/result.json` reports four million native CALLs in
each warm call/return sample (out of 14,000,004 guest instructions), with matching
checksums and instruction counts. Its 32-bit sample took about 292 ms; the earlier
303 ms observation is not a controlled repeated comparison. This is partial
control-flow coverage, not evidence that the CALL/RET bottleneck is resolved.

Near RET and RET imm16 now use the guarded POP read path and end the block at
the value read from the guest stack. Stack adjustment is committed only after
a successful read, using SS.B independently of operand width; large imm16 cleanup
values use register arithmetic rather than the ARM immediate field. Differential
checks cover both code/operand/stack widths, paging, cross-page reads, stack
faults, MMIO, and cleanup values through 65535. The initial run at
`build/guest-micro-native-return/result.json` checks all twenty micro intervals
and returns to DOS. Warm call/return samples execute 12,000,000 of 14,000,004
instructions natively; the 32-bit sample took about 132 ms. These individual
observations identify increased native coverage, not a repeated performance
comparison or an application speedup claim.

The native-return change also passes `make test`, `make smoke-guest-perf` and
`make smoke-win311` (Notepad saves exact contents through Windows/DOS). Single
snapshot task runs for the Win95 calculator, Word 97 and Excel 97 reach their
recorded framebuffer states. These are compatibility checks, not startup
performance comparisons. Evidence is in `build/native-return-{tests,smoke,win311}.log`
and `build/native-return-{win95-calc,word,excel}/result.json`.

## Counter branches after native returns

Fresh host-stack and opcode profiles after CALL/RET lowering are retained at
`build/post-return-{word,excel}-profile/run-0/`. Both tasks reached their expected
pixels. The profiles counted 368,294 interpreted LOOP instructions in Word and
144,596 in Excel. Instrumented timings include profiling overhead and are not
performance comparisons. Interpreted dispatch remains substantial; these counts
do not establish that LOOP is the dominant whole-application cost.

Native LOOP and JCXZ/JECXZ now terminate translated blocks while preserving
flags. Address size chooses CX versus ECX independently of the operand size
used for the branch destination. Differential tests include zero/one counters,
16-bit wrap, nonzero upper ECX bits, forward/backward jumps, and branch targets
that wrap past 64 KiB. LOOPE and LOOPNE remain interpreted.

Microkernel IDs 21/37 exercise the native LOOP path with a checksum of
`3 * loops` modulo the accumulator width. Both use ECX, including an address-size
prefix in the real-mode kernel, so the default million-iteration run does not
silently truncate the count to 16 bits. At that stage the complete suite had 24 intervals.

Validation evidence: `build/guest-micro-native-loop{,-ref}/result.json` uses the
same COM bytes across both engines, with all 24 checksums and architectural
instruction counts matching. `build/native-loop-{tests,smoke,win311}.log` records
the unit suite, odd-count DOS matrix/CRC rejection checks and exact Win3.11 file
save. Win95 calculator and Office compatibility task results are retained under
`build/native-loop-{win95-calc,word,excel}/`; their timings are not used to claim
an application performance improvement.

## Register extension instructions

The post-return profiles counted 159,217 register-source MOVZX-byte attempts in
Word and 86,144 in Excel, plus word-source and signed-extension forms. Native
register MOVZX/MOVSX now accept byte/word sources and independently selected
16/32-bit destinations. AH/CH/DH/BH are extracted before writing the destination,
including when source and destination overlap. Flags remain unchanged, so a
preceding arithmetic operation still supplies subsequent Jcc conditions.

The differential byte suite covers all source/destination registers, both code
sizes, operand overrides, signed/unsigned extension, sign-boundary values and all
16 Jcc conditions. At that stage memory-source forms remained interpreted; the subsequent change
below adds guarded reads. Register coverage alone does not establish an
application speedup. Near Jcc decoding remains supported alongside the new 0F
opcode forms.

Post-change host/opcode profiles and framebuffer task results are retained at
`build/native-extend-{word,excel}-profile/`. Word records 116,583 interpreted
register MOVZX-byte attempts and 23,975 register MOVZX-word attempts; Excel
records 69,012 and 14,910 respectively. Cold addresses and short blocks still
interpret. The earlier post-return profile also preceded LOOP lowering, so
these counts are evidence of current coverage, not an isolated speed comparison.

`build/native-extend-tests.log` records the full regression pass, including
1,227,548 byte-JIT checks. DOS CoreMark CRCs and the 24-interval micro matrix pass
under both engines (`build/native-extend-smoke.log`). Win3.11 saves exact Notepad
contents (`build/native-extend-win311.log`), and the Win95 calculator snapshot
reaches its recorded result (`build/native-extend-win95-calc/result.json`).

## Guarded memory extension instructions

Memory-source MOVZX/MOVSX now reuse the native RAM-read guards, permission checks
and interpreter replay of existing loads. The access width is independent of the
destination width: byte/word reads access exactly one/two bytes, then zero/sign
extend into a 16/32-bit destination. This distinction applies to page and RAM
bounds, host load opcodes and slow-helper metadata. The destination changes only
after a successful read; earlier arithmetic flags remain restartable.

The read differential matrix now includes all four extension opcodes, source
sign boundaries, 16/32-bit destinations and addressing, segment overrides, all
destination registers and Jcc conditions. Additional cases cover page/segment
faults, cached supervisor-only translations, MMIO read counts, cross-page reads,
source accesses ending at the last RAM byte and EAX/ESP address/destination
aliases. The previous instruction's flags and the original fault IP must match
the interpreter. These checks target correctness and do not establish a broad
performance improvement.

Validation logs: `build/memory-extend-{tests,smoke,win311}.log`. The read-JIT
suite passes 156,497 checks; DOS CoreMark CRCs, all 24 micro intervals and
Win3.11 exact Notepad file saving pass. Updated Word/Excel host and opcode
profiles are at `build/memory-extend-{word,excel}-profile/`, and the Win95
calculator reaches its recorded pixels. These runs are compatibility/coverage
checks, not a controlled application-performance comparison.

A fresh COW child of the original `media/rtts-win95.img` cold-boots to an
unobscured Explorer desktop under this backend. Evidence:
`build/memory-extend-rtts-cold-tskq3zv6/verification/result.json`. The existing
checker dismisses the clock/display dialogs; its unrecognized DHCP warning was
closed with guest Tab/Enter selecting No. This verifies the cold boot but is not
an unattended-helper pass. The private compressed disk was removed on success;
the original image was only used as its read-only parent.

## Register exchanges

The memory-extension profiles still counted 288,046 register-byte XCHG attempts
in Word and 66,566 in Excel. Register XCHG now runs natively for byte, word and
dword operands, including the accumulator short forms. Both old operands are
captured before either destination is changed, so AL/AH aliases and self-exchange
preserve every unaffected bit. Guest flags do not change. Memory XCHG remains
interpreted; no memory atomic-operation semantics are added by this change.

The byte differential suite checks all ModR/M register combinations, code sizes,
operand overrides, self-aliases and accumulator encodings. A later segment fault
also verifies both exchanged registers and the preceding arithmetic flags.

Guest microkernels 22/38 execute `XCHG AL,AH; ADD AX,1; XCHG AL,AH` followed by a
32-bit-count LOOP. The addition prevents a pair of skipped exchanges from
producing the intended result in general. The checksum is the low 16 bits of
the iteration count with its bytes exchanged, computed independently by the
host. Both execution modes use byte exchanges and a word accumulator. JSON records
`codeBits` separately from `operandBits` (8), `accumulatorBits` (16), and
`counterBits` (32) for this kernel. The suite now has 28 intervals.

Validation: `build/register-xchg-tests.log` records the full unit pass, including
1,271,327 byte-JIT checks. The final odd-count guest smoke is
`build/guest-perf-smoke-8nq656hw/result.json` (56 intervals across both engines,
plus CoreMark CRCs and intentional wrong-result rejection). Million-loop results
in `build/register-xchg-byte-{jit,ref}/result.json` match every checksum and
architectural instruction count. Each warm exchange sample executes all four
million loop instructions natively. Observed times of roughly 12–13 ms versus
76–79 ms interpreted are single diagnostic runs, not a repeated performance
comparison or an application speedup.

Win3.11 exact file saving passes (`build/register-xchg-win311.log`). Updated
Word/Excel profiles reach the recorded pixels at
`build/register-xchg-{word,excel}-profile/`. Both original-rtts and installed-OSR2
cold boots and guest restarts pass with private disk removal:
`build/win95-smoke-nryolhk7/result.json` and
`build/win95-smoke-9pt0r57b/result.json` respectively.

## Repeated host-timed comparisons

```sh
# Three alternating pairs, each final CoreMark interval >=10 seconds:
python3 tools/compare_guest_perf.py --repeat 3 --output build/coremark-pairs
# Keep each microkernel and cold/warm state separate; five million loops:
python3 tools/compare_guest_perf.py --suite micro --repeat 3 --output build/micro-pairs
# Quick validation of the comparison pipeline; no eligible CoreMark scores:
python3 tools/compare_guest_perf.py --iterations 10 --repeat 2 --output build/pairs-check
```

The tool freezes one guest-bench executable in its output directory, then
alternates JIT/interpreter ordering by pair. Every run independently boots DOS,
checks the original guest result and returns to DOS. Guest COM hashes must match
across all runs; runner hashes must match the frozen executable. Micro checksums,
work amounts and architectural instruction counts must also agree across both
engines and every repetition. Passing runs remove their compressed private disks.
The output directory must be new; failed runs retain diagnostics and no completed
aggregate is written. `runs.json` preserves validated partial progress.

CoreMark compares iterations per host second because automatic calibration may
choose different iteration counts across engines. Default runs must pass the
upstream minimum runtime as well as CRC validation. Fixed iteration mode is
reported as diagnostic. Micro cases retain separate physical units and cache
states; no heterogeneous overall score is calculated. Each row reports raw
samples, median/min/max rates, and paired JIT/interpreter speedup ratios.
Three pairs are a small sample; ranges are observations, not confidence bounds.
Host load and scheduling can affect wall times and are recorded in each raw run.
This comparison establishes neither hardware equivalence nor Rosetta/application
performance. `--runner PATH` on the single-run tool permits an existing runner
without rebuilding it and records its path and hash.

Current three-pair validated CoreMark evidence is retained at
`build/guest-coremark-repeated/result.json`. JIT rates were 267.1, 255.3 and
251.1 iterations/host-second (median 255.3); interpreter rates were 119.7, 120.4
and 119.0 (median 119.7). The median of the three paired speedup ratios is 2.12x,
with observed ratios 2.11–2.23x. Every final interval exceeded ten host seconds,
passed the canonical CRC checks, and returned to DOS. Native coverage in the
final JIT intervals was about 73.9%. This is a current JIT-versus-interpreter
comparison, not a controlled comparison against earlier emulator revisions.

`build/guest-comparison-micro-check/result.json` validates two alternating pairs
of all 28 intervals at 17 loops, including matching checksums and architectural
counts. Its short rates are not performance evidence. The separate short
CoreMark comparison at `build/guest-comparison-short-check/result.json` remains
explicitly ineligible for a validated score. All fourteen independent DOS runs
used frozen runner binaries within their comparison and removed their private
disks after success. Invalid `--runner` input is rejected before disk creation.

## INC/DEC and preserved carry

Native register INC/DEC supports byte ModR/M, word/dword ModR/M and short
register encodings. The result is computed in cached ARM64 registers. Flag
materialization uses private INC/DEC operation tags that preserve CF while
updating the remaining arithmetic flags. A pending earlier ALU or memory-CMP
flag result is materialized before INC/DEC consumes its carry, including chains
and later full flag overwrites. Two-instruction INC/DEC + Jcc blocks can warm
into native code. Memory read/modify/write forms remain interpreted.

The differential suite covers every byte value with both carries, high-byte
registers, 16/32-bit sign/overflow boundaries, all Jcc conditions, pending
ADD/SUB/CMP carry chains and restart state at a following segment fault.
`build/incdec-tests.log` passes the full suite, including 1,870,373 byte checks.
`build/incdec-smoke.log` passes DOS CRCs and all 28 micro intervals under both
engines; `build/incdec-win311.log` verifies exact Notepad file saving.

The controlled comparison at `build/incdec-controlled/` freezes all source files
and compiler arguments for two runners and changes only `src/jit.cpp`. Its build
manifest records both JIT-source hashes and common-source hashes. Three
alternating, independently booted CoreMark pairs pass upstream minimum runtime
and CRC checks. Baseline median throughput is 254.6 iterations/host-second;
INC/DEC median is 294.1. Paired ratios are 1.155, 1.170 and 1.163 (median about
16.3% improvement). Native coverage rises from 73.9% to 79.4%. This measures this
specific change; it does not establish Rosetta-level throughput.

Million-loop baseline/candidate micro results at
`build/incdec-micro-{baseline,candidate}/result.json` retain matching checksums
and architectural counts. Warm ALU and CALL/RET loops now execute their counter
DEC/Jcc instructions natively too. Their timings are single diagnostics, not
repeated comparisons. Updated cold boot and guest restart checks pass for both
sources: `build/win95-smoke-7qf2q245/result.json` (rtts) and
`build/win95-smoke-faunui67/result.json` (installed OSR2), with private disk removal.
