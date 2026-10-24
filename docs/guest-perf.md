# CPU performance measured inside a DOS guest

```sh
python3 tools/bench_guest_perf.py --output build/coremark-jit
python3 tools/bench_guest_perf.py --no-jit --output build/coremark-interpreter
# Fast checksum/replay diagnostics, not reportable CoreMark scores:
python3 tools/bench_guest_perf.py --iterations 10 --output build/coremark-quick
```

Each command creates a private **compressed COW disk** against `media/dos.raw`,
replaces CONFIG/AUTOEXEC only in that child, and boots actual MS-DOS. No raw
2 GiB disk copy, mount, Windows startup, screenshot or OCR is involved. A passing
run removes its private disk; a failed run keeps it and its log for inspection.
Results, program binary, object files and exact build options remain under the
requested output directory. The directory must not contain an existing disk.

The original 12 KiB COM loader disables interrupts, masks the PIC, enables A20,
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
| E5 | Reserved for case results |
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

This suite currently isolates 32-bit integer execution, stack/calls/branches and
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
