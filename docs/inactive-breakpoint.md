# Skip inactive breakpoint translation

The main loop previously called `CPU::address(CS, ip)` before comparing with the
breakpoint address even when its disabled value was `0x100000000`. Since the
translation returns a 32-bit linear address, that comparison cannot match.
The loop now checks that a breakpoint fits the 32-bit domain before translating.
Active breakpoint matching and the after-break trace path are retained.

The finer Excel diagnostic attributes 283 direct-main `CPU::address` self samples
out of 14,742 main-thread task samples (1.92%) to the old call. Its immediate
sampled parent is `main.cpp:333` in the frozen reference. This is a wall-stack
share, not exact CPU duration or a prediction of the isolated speedup.

## Validation

A frozen matched-source candidate differs only in this main-loop condition.
Actual headless CLI runs verify:

- DOS 6.22 reaches the version and ready marker with JIT and interpreter.
- `--break-at 0x7c00` stops at boot entry with zero executed instructions.
- `--no-jit --break-at 0x7c00 --after-break 3 --trace ...` produces exactly
  three instructions and three trace entries.

The current repository `pcemu` CMake target also compiles. CPU/JIT lowering,
cache size and dispatch policy are unchanged. The temporary DOS child is removed.

Three alternating uninstrumented pairs per Office task pass every expected
pixel gate. Excel CPU ratios (baseline/candidate) are 1.123, 1.162 and 0.902;
Word ratios are 1.030, 1.397 and 1.199. Their medians are 1.123 and 1.199, but
the shared-host spread is wide and Excel has an unfavorable pair. These are
exploratory measurements, not stable expected gains or isolated attribution of
the whole observed difference to one removed call. No application speedup
percentage is claimed as a general result.

The change removes measured unnecessary work without changing the tested
normal/debugger outcomes. No new large guest/Office fixture or candidate-specific
fresh Setup run is made. The broader useful-work target remains unachieved.

[Exact source/build/binary identity, CLI states/logs, profile counts, drivers and
all six Office pairs](measurements/inactive-breakpoint-2026-10-26.json).
