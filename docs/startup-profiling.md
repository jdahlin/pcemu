# Word and Excel startup profiling

Measured on the local Apple M4 Mac on 2026-10-24. Both applications launch
from their existing post-boot Windows 95 checkpoints. The workload includes
Windows loader, filesystem, graphics and interrupt activity until the first
pixel-verified rendered Word page/section status or Excel Ready status.

## What the profiles show

Five host traces per application, before the fixes, contain 10,247 Word and
4,788 Excel main-thread samples:

| Sampled main-thread work | Word | Excel |
| --- | ---: | ---: |
| Interpreter, including MMU and guest faults | 45.6% | 44.4% |
| JIT dispatch, cache lookup and validation | 16.8% | 16.1% |
| Screenshot/video/control output | 11.7% | 12.9% |
| Device polling and interrupt delivery | 10.8% | 9.5% |
| JIT compilation, including decoder exceptions | 7.0% | 9.5% |
| Generated code and native helpers | 2.9% | 2.5% |
| Disk and compression | 0.8% | 0.6% |
| Main loop and other host work | 4.5% | 4.5% |

These are exclusive categories computed from complete call trees, not sums of
overlapping inclusive percentages. `CPU::physical` appears in about 12–13%
of stacks across its callers; that overlaps the interpreter/dispatch totals.
Host exception throwing/unwinding appears in 5.7% / 7.9% of stacks. A large
part comes from the JIT using `throw 0` to stop decoding ordinary unsupported
instructions, rather than an actual guest exception.

Three additional **SDL frontend** traces per application, after the fixes,
contain 6,091 / 2,863 task samples. Interpretation remains 43.9% / 41.0%, JIT
dispatch 16.7% / 17.3%, device polling 9.9% / 9.2%, and disk/compression
0.8% / 0.6%. SDL rendering/window events account for 1.8% / 3.5%; framebuffer
scanout, screenshot capture and control output account for another 16.6% /
16.8%. The frequent captures are benchmark overhead, so they should not be
mistaken for guest CPU execution. Compilation is now 1.8% / 2.4% in these
traces. Different frontend/profile boundaries make these percentages useful
for ranking remaining work, not an exact before/after CPU-time subtraction.

The SDL worker stacks are reported separately: 81,176 of 81,358 Word worker
samples and 61,699 of 61,793 Excel worker samples are blocked waits. Including
those waits in the main-thread denominator would hide the actual bottlenecks.

## Guest instruction and dispatch counts

A separate instrumented executable records exact opcode attempts, ModRM
register/memory forms, groups, JIT dispatch outcomes and disk activity. Three
runs per application counted 99.1 million Word and 43.6 million Excel
interpreted attempts. About 87.2% / 85.3% of JIT dispatches reach cached
entries that remain interpreted. Native blocks execute multiple instructions,
so this is distinct from the roughly 31% native instruction coverage.

Common interpreted operations include:

| Primary opcode | Word attempts | Excel attempts |
| --- | ---: | ---: |
| MOV r,rm (`8B`) | 7.9% | 8.1% |
| ALU rm,imm8 (`83`) | 5.5% | 5.1% |
| JZ + JNZ | 10.4% | 9.9% |
| INC/DEC/CALL/JMP/PUSH group (`FF`) | 4.3% | 4.9% |
| Near RET (`C3`) | 2.8% | 3.1% |
| CMP r,rm (`3B`) | 2.8% | 2.8% |
| SUB r,rm (`2B`) | 2.7% | 3.0% |
| Relative CALL (`E8`) | 2.3% | 2.7% |

MOV and conditional branches already have lowering. Their interpreted counts
also expose cold or short blocks cut by unsupported operations; adding a new
opcode alone will not remove all these costs. Many `FF` calls/pushes and CMP,
SUB, TEST forms use memory operands. Extended-opcode totals are broken down
in the raw reports rather than double-counted with the primary `0F` prefix.
Guest IP samples use jittered intervals and include CR3, CS base and selector
to keep different address spaces distinct. They are not symbolized Word or
Excel function profiles.

## Changes and measured result

Two fixes follow directly from the profiles:

1. Common unsupported JIT instructions end decoding with normal control flow.
   Real guest faults and rare speculative page-boundary exits retain their
   existing exception/fallback handling.
2. Device polling computes the slave PIC priority once per poll, skips scans
   when no unmasked request exists, and avoids checking in-service priority
   when the PIC has no request. Poll frequency and guest interrupt deadlines
   remain unchanged.

An unprofiled comparison alternates baseline/candidate execution order, with
five independent restores **per application and version using the SDL
frontend**:

| Application | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Word | 2.524 s | 2.209 s | 12.5% |
| Excel | 1.210 s | 1.023 s | 15.4% |

Execution-batch medians also improve: Word 2.024 to 1.783 s, Excel 0.954 to
0.798 s. These exclude capture, SDL presentation and control work but include
host preemption. End-to-end samples include those costs. Host caches are not
flushed, and five pairs do not establish a confidence interval or Rosetta
parity. All samples, including slower first runs, are retained.

The suite passes 541,340 checks, including additional interrupt-priority
coverage for rotation, masks, special masks, nesting and empty requests.

## Next work, in priority order

1. **Reduce interpreted dispatch/decode/MMU overhead.** Most dispatches still
   interpret one instruction and repeat cache/mode checks. A decoded-block
   interpreter or guarded block chaining could improve already-supported MOV
   and branch paths while retaining permission, remapping and SMC checks.
2. **Memory ALU and control-flow lowering.** CMP/SUB/TEST memory forms, calls,
   returns and indirect control flow both cost interpreter work and split
   native blocks. Preserve precise stack/fault behavior when expanding them.
3. **Continue reducing device-poll cost without delaying interrupts.** It is
   still about a tenth of sampled startup work. Event deadlines would need
   explicit invalidation on port I/O, keyboard input and PIC changes.
4. **Separate observation cost from application latency.** Capturing a whole
   VGA frame every few milliseconds costs more than SDL presentation here.
   Less intrusive readiness probes can improve measurement quality, but that
   should be reported separately from emulator execution improvements.

Disk decompression and a different compression format are low priorities for
these startups: disk/compression is below 1% of sampled work.

## Reproduce and inspect

```sh
make build/pcemu-profile
# Host stacks, including the real SDL frontend:
python3 tools/bench_snapshot_task.py build/snapshots/office97-excel-startup \
  --task build/snapshots/office97-excel-startup/task.json --repeat 3 \
  --sample --gui --output build/excel-host-new
python3 tools/analyze_samples.py build/excel-host-new \
  --output build/excel-host-new/summary.json
# Guest opcode/dispatch diagnostics (do not use their timings as benchmarks):
python3 tools/bench_snapshot_task.py build/snapshots/office97-word-startup \
  --task build/snapshots/office97-word-startup/task.json --repeat 3 \
  --guest-profile --output build/word-guest-new
python3 tools/analyze_guest_profiles.py build/word-guest-new \
  --output build/word-guest-new/summary.json
# Unprofiled, alternating before/after comparisons:
python3 tools/bench_compare.py --baseline build/pcemu-before-whole-profile \
  --candidate build/pcemu-after-whole-profile --gui --repeat 5 \
  --output build/startup-comparison-new
```

Current evidence: `build/profile-{word,excel}-host/summary.json`,
`build/profile-{word,excel}-guest/summary.json`,
`build/profile-{word,excel}-gui-after/summary.json`, and
`build/profile-comparison-sdl/result.json`. Frozen executables preserve the
comparison. Instrumentation hooks compile away in the normal executable.

The host sampler attaches after checkpoint readiness. A named three-second
guest-side wait lets symbol collection finish without executing the guest;
analysis excludes its stack samples. This matters for SDL's many frameworks:
an early pilot ended before sampling began. Those incomplete GUI pilot traces
are not evidence for the percentages above. Collection ends when the guest
quits immediately after readiness, using `sample -mayDie` to retain symbols.
Initial headless traces predate the named barrier and have small attach/exit
margins. CPU-side wall-stack sampling does not measure GPU utilization, and
the first status-bar match does not imply all background initialization ends.


## 24 October: native memory comparisons

The original ARM64 emitter now lowers byte/word/dword memory CMP and TEST:
register/memory CMP in either direction, memory/immediate CMP (including signed
imm8), memory/register TEST, and memory/immediate TEST. These use the existing
single-page RAM/TLB guards. MMIO, crossing accesses, unusable segments and page
faults retain interpreter replay. Failed accesses publish the preceding flags;
successful comparisons publish their own flags before a later fault or SMC exit.
No guest register or memory write is introduced by the comparison.

`tests/jit_read_test.cpp` adds 102,804 differential checks over all Jcc conditions,
8/16/32-bit operands, 16/32-bit address/operand overrides, high-byte operands,
FS/GS and SS-based addressing, SIB/register aliases, permission faults, MMIO,
cross-page reads, subsequent faults, code modification and arena recycling.
The full suite passes **644,288** checks. Win3.11's Notepad exact-save test and
all three MS-DOS firmware/controller reset tests also pass.

Five alternating unprofiled SDL pairs, using the same older VGA fixtures as the
previous work, produced:

| Application | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Word | 2.391 s | 2.242 s | 6.2% |
| Excel | 1.080 s | 0.989 s | 8.4% |

CPU-batch medians are Word 1.896 to 1.772 s and Excel 0.840 to 0.762 s.
Every run restored an independent private disk and passed its rendered-pixel
readiness check. An existing GUI emulator was running throughout the comparison;
there was no simultaneous build/test work in these timed pairs. This is a paired
measurement under that host load, without confidence intervals or Rosetta parity.
The new high-resolution networked checkpoint is not the timed fixture.

Evidence: `build/memory-compare-startup-isolated/result.json`, frozen binaries
`build/pcemu-before-memory-compare` and `build/pcemu-after-memory-compare`, and
`build/memory-compare-tests.log`. The earlier `build/memory-compare-startup/`
pilot overlapped compilation/testing and is retained but excluded from the table.

Fresh diagnostic guest profiles are under `build/profile-{word,excel}-memory-compare/`.
Their instrumented timings must not be used as startup performance results.


## 24 October: short comparison blocks and immediate register TEST

Production warmup now admits two-instruction blocks containing CMP/TEST followed
by a conditional branch. Previously the minimum block length rejected these
permanently even after they became hot. Memory guards, instruction counting,
single-step and interrupt-shadow handling are unchanged. Immediate register TEST
forms (`A8/A9`, `F6/F7 /0`) also lower to native code, preserving high-byte and
partial-register operands.

Five alternating SDL pairs for **memory comparison/branch pairs alone** gave:

| Application | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Word | 2.132 s | 2.021 s | 5.2% |
| Excel | 0.968 s | 0.932 s | 3.7% |

A second five-pair comparison added **register comparison/branch pairs and
immediate register TEST**, against that first candidate:

| Application | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Word | 2.205 s | 2.177 s | 1.3% |
| Excel | 1.058 s | 0.987 s | 6.8% |

The latter CPU-batch medians are Word 1.607 to 1.535 s and Excel 0.711 to
0.688 s. These are separate paired experiments: do not multiply their percentage
reductions or compare absolute times across the tables. Host caches are not
flushed. The second experiment includes first-pair Word outliers of 12.117 s
and 4.837 s; all samples are retained. Five pairs do not establish confidence
intervals, and Word's second end-to-end difference is small. No other emulator
was observed running during these measurements; no build/test jobs overlapped
any reported timed pairs. The fixtures are still the original 640x480 VGA ones,
not the native high-resolution desktop, and Rosetta parity remains unproven.

An initial second-stage pilot failed its readiness match with the Windows Start
menu covering Word's status bar (`build/register-test-startup/failed.png`). Word
had actually started; the overlay's source was not established. That failed run
is excluded. GUI automation now uses `--no-host-input` when the engine advertises
it, so physical host keyboard/mouse events cannot steer the benchmark guest.
Virtual control-file input, window close and Cmd+Q remain available. Normal
interactive launches retain host input. `host-input on/off` can change this policy
through the existing control file. State JSON and task result JSON record it.
Legacy frozen engines without the option remain supported and report that input
suppression was unavailable.

For the second comparison, both engines were built with the same updated frontend
and input policy; the baseline excludes just the new register TEST lowering and
register-pair admission. Sources and the exact baseline compiler argv are retained
in `build/register-test-sources/`. Frozen executables are
`build/pcemu-before-register-test-controlled` and
`build/pcemu-after-register-test-controlled`. Every successful raw GUI result
reports physical host input suppressed. Evidence:
`build/short-compare-startup/result.json`,
`build/register-test-startup-controlled/result.json`, and
`build/short-compare-validation.json`.

The read/compare differential suite now has 110,493 checks, including production
warmup and register TEST across all Jcc conditions, widths and high-byte registers.
The full suite passes **651,977** checks. Windows 3.11's exact Notepad save and
all three DOS reset/reboot paths pass again. Fresh instrumented SDL diagnostic
profiles are `build/profile-{word,excel}-short-compare/`; their timings are not
performance evidence.


## Refreshed compact-TLB headless profile (2026-10-24)

Three independent headless host samples per application use the frozen
`build/aligned-tlb-comparison/baseline` executable, which includes combined
TLB keys and block-local epoch retention, with default jump joining. This is
the compact layout, without the rejected 32-byte alignment or the subsequent
REP STOS experiment. Executable/task hashes are in the result manifests.

| Main-thread category | Word | Excel |
| --- | ---: | ---: |
| Interpreter, including MMU and faults | 31.0% | 32.7% |
| JIT dispatch/cache/validation | 21.1% | 20.6% |
| Capture/video/control output | 13.0% | 11.6% |
| Generated code and native helpers | 10.3% | 6.8% |
| Compilation | 6.2% | 9.5% |
| Device polling and interrupt delivery | 6.3% | 6.9% |
| Disk and compression | 1.4% | 0.8% |
| Other main-thread work | 10.6% | 11.2% |

The reports contain 5,016 Word and 2,129 Excel task samples after excluding
the attachment barrier. `CPU::physical` appears inclusively in 11.38% and
11.74% of stacks, overlapping the categories above. These are main-thread
wall-stack samples, not exclusive CPU-clock measurements or a before/after
speed comparison. Generated code is partly unsymbolized. Profiling overlapped
an independent test build, so host contention is another limitation. No
profiled latency is used as a benchmark. All six tasks passed pixel checks.

Raw stacks and analysis are under `build/current-mmu-word-host/` and
`build/current-mmu-excel-host/`, with `summary.json` in each directory.
The evidence supports work on both translation and dispatch, but does not
by itself show which guest string operations dominate interpreter time.


The concurrent Win11-only storage cleanup subsequently removed these raw
profiles, manifests and analysis files. The cleanup inventory is
`build/storage-cleanup/win11-only-cleanup.json`. The figures above were read
from completed analyzer output before removal, but the underlying traces are
no longer available for independent inspection. Treat them as recorded
observations, not a currently reproducible evidence bundle. The cleanup also
removed the Word/Excel startup checkpoints used for these measurements.


## Current headless Windows 95 boot host profiles (25 October)

The retained rtts and installed OSR2 images now have fresh host call-tree
profiles with the current native NEG and memory-source ALU implementation.
These use the ordinary headless emulator, not an instrumented build or restored
RAM checkpoint:

```sh
python3 tools/smoke_win95.py --sample
python3 tools/smoke_win95.py --sample --disk media/win95-fresh-installed.pcd
# Also collect through the guest-requested restart:
python3 tools/smoke_win95.py --sample --restart
```

The first control pause protects early startup during symbol discovery; a second
named pause is queued after the sampler confirms attachment. Analysis requires
that a named pause appears in the stacks, excludes its samples, and rejects
sampling that terminates before desktop verification. `sample -mayDie` collects
until the owned guest exits. The emulator hash is checked before/after the run.
Successful compressed overlays are removed. The analyzer recognizes native
helpers in both their former `pc::native*` and current `pc::jit_detail::native*`
namespaces, so their work does not inflate dispatch accounting.

| Main-thread category | rtts cold boot | OSR2 cold boot |
|---|---:|---:|
| Interpreter, including MMU and guest faults | 50.7% | 51.2% |
| JIT dispatch/cache/validation | 16.7% | 18.0% |
| Device polling and interrupt delivery | 13.2% | 12.1% |
| Main loop/other work | 9.0% | 8.3% |
| Generated code/native helpers | 8.3% | 7.7% |
| JIT compilation | 1.6% | 1.9% |
| Disk/compression | 0.4% | 0.5% |
| Capture/video/control output | 0.2% | 0.2% |

The cold-boot profiles contain 7,757 and 5,113 task samples, after excluding
4,617 and 4,628 attachment samples. `CPU::physical` accounts for 12.9%/11.3%
self samples, `Jit::run` 13.4%/14.8%, and `I8042Device::poll` 6.6%/5.3%.
Exception throwing has 5.9%/6.8% inclusive samples; those overlap interpreter
work and are not an independent category to add. This supports investigating
software-MMU/interpreter hot paths and polling overhead before disk work.
Guest faults must retain architectural behavior when reducing exception cost.

These are one host profile per cold boot, plus a separate rtts boot/restart
profile that also passed. The windows include known startup-dialog dismissal,
OCR observation, guest execution while the observer works, and small attachment/
exit margins after the first dispatch batch. Unresolved generated stacks can
appear under other work. They are wall-stack samples, not exclusive CPU-clock
counts, isolated boot times, or a before/after/Rosetta comparison.
[Complete summaries and verification](measurements/win95-host-startup-2026-10-25.json)
include hashes and paths to three committed zstd-compressed raw call trees
(219,048 compressed bytes total). Decompress each with `zstd -d -c FILE` to audit
the original stacks. This preserves the evidence independently of local build
cleanup. Four host-accounting regression tests and the normal unprofiled Win95
restart path validate the profiling tool changes.

## Bounded whole-machine opcode profiles

Build `make build/pcemu-profile build/pcemu build/disk-tool`, then run
`python3 tools/smoke_startup_profile.py`. This uses existing rtts media and two
small private compressed overlays. Both engines stop at 300 million retired
instructions; captured CPU/device state and JIT counts must agree. Successful
children are removed. Failures preserve their disks for diagnosis.

`build/pcemu-profile --guest-profile FILE` records interpreter opcode/operand
attempts and dispatch counters from entry into the execution loop through normal
exit. VGA BIOS initialization precedes this interval, so startInstructions can
be nonzero. The normal executable rejects this option. Instrumented timings are
not benchmarks. Native replay contributes interpreter attempts; faulting dispatch
can exit before selecting native or interpreted execution. Existing interactive
profile-start/profile-stop commands can replace the active diagnostic interval.

## Separate application-use intervals

`python3 tools/smoke_win95.py --applications --guest-profile` uses the diagnostic
executable and starts each interval only after the application's window has been
verified. Calculator covers 2+2 evaluation; Notepad covers clipboard paste,
editing, and saving. Launch, boot, shutdown, and restart are outside these opcode
intervals. The usual exact file readback and restart checks still apply.

The output directory contains `application-profiles/calculator.json` and
`notepad.json`. Counts include guest idle/message loops and input settling;
they are not host execution costs, isolated latency measurements, or evidence
about Office performance. Use these alongside boot profiles and CPU/memory
kernels rather than treating startup as representative of application use.
