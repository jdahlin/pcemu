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
