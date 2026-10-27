# Profiling the 50-frame Quake workload

```sh
./run-quake.sh --profile --frames 50 --repeat 5 --host-repeat 3
# Keep a named, reproducible collection:
python3 tools/profile_quake.py --output build/quake-jit-before
```

The default is the JIT. Use `--backend interpreter` for a separate reference
collection. The 50-frame prefix is the development and profiling workload.
Repeat this short workload for more samples; defer full demos until a speed
milestone. A 5–10x speedup is a target, not a prediction.

## Match the execution path

Both `pcemu` and `quake-bench` call `runPcBatch` from `pc_execution.hpp`:
`Jit::runBatch(64)`, a device poll after each batch, and the session's additional
50,000-step polling checkpoint. The benchmark uses the Quake launcher's eager
compilation, same-page jump joining, 16 MiB arena and disabled experimental direct
links. Reports record these settings and reject mixed baseline configurations.
Frontend presentation, control and capture are excluded from the timed runner.

The benchmark retains its external monotonic PIT clock at 1,193,182 Hz so the
original DOS timedemo FPS reflects host time. Ordinary sessions still have their
instruction-derived PIT clock. This difference is explicit in the report; it is
not a claim that every production device clock has changed.

## Measure the exact frames

The original DOS Quake 1.06 executable must have SHA-256
`7cdb244122cc607cc6f162524bfcac60f6d11d8ca2ff07473d6cd962311161fb`.
The wrapper checks it offline. Runtime fingerprints and mode checks identify:

- After EIP `00003ac3` stores `cls.td_starttime`, EIP `00003ac9` begins the
  interval. `host_framecount == td_startframe + 1` excludes the warmup frame.
- EIP `00003f86`, entry to `CL_FinishTimeDemo`, ends the interval before reporting.

The mapping follows [Quake's timedemo implementation](https://github.com/id-Software/Quake/blob/master/WinQuake/cl_demo.c)
and the pinned original executable. Native decoding ends blocks before these
linear addresses. Both root and cached batch dispatch stop before executing an
observation point; the next batch executes it normally. Direct links are rejected
with observation points. No guest instructions are patched.

The host frame delta must equal the final guest-reported frame count. The stopped
guest's console log must confirm that result and its DOS file must be closed.
Scratch overlays are deleted after successful validation. Guest images and game
assets remain unchanged.

Boundary checks and cheap batch counts are common runner overhead, retained in
all scores. Clocks are read at the two boundaries, not at every instruction.
The window includes device polling and guest OS work during the timed frames.
Native step counts include compiled helper execution; they are not counts of
inline ARM64 operations or architecturally retired x86 instructions. REP step
units depend on the emulator's bounded continuation scheme.

## Separate speed, sampling and counters

Each collection freezes nonignored build inputs, including working-tree source
edits, in its own directory. The manifest records per-file hashes, Git state,
compiler/version/flags, binary hashes, backing-image hashes, host and load.
Concurrent edits during capture fail; later edits cannot alter frozen binaries.
The optimized build uses `-O3 -DNDEBUG -g -gdwarf-4 -fno-omit-frame-pointer` and
an adjacent macOS dSYM. Keep sources, objects, symbols and traces together.

Runs are one excluded host-cache warmup, a baseline, separate Instruments
collections, separate guest counters, then the remaining baselines. Each is an
independent boot with a small temporary overlay of the existing Quake image.
This measures a first timedemo pass per boot, not a warmed second pass in the
same JIT cache. Do not mix those different cache histories in comparisons.

Only uninstrumented `baseline-*` runs contribute FPS. Report wall and thread-CPU
milliseconds/frame, median, MAD, CV, range, CPU/wall ratios and drift. Rounded DOS
FPS is preserved, but host scores use the high-resolution exact frame window.
Five samples are an initial baseline, not a confidence guarantee. Host contention
and high variance generate warnings.

Instruments attaches while the guest waits just before the start boundary. The
attachment pause is removed from PIT time. CPU samples are filtered to the exact
Unix-nanosecond frame window and verified main-thread PID. Timestamped JIT maps
track code publication and arena generations; recycled addresses are never
resolved from an aggregate call tree. Samples near publication/recycle boundaries
are conservatively left unmapped and reported explicitly.

Exclusive CPU categories conserve the sample total: compilation, generated code,
dispatch/validation, typed x87/string/arithmetic/segment helpers, clocks, polling,
interpreter and other host work. Typed helpers execute outside generated frames,
so symbol ancestry identifies them separately from generic `CPU::step()`.
Generated regions and guest-block rankings are additional attribution, not
percentages to add to the exclusive categories. Map-recording overhead is
instrumentation; sampled FPS is never a baseline score.

The collector repeats the 50-frame host workload to build a useful sample count.
Fewer than 3,000 CPU samples or more than 1% unmapped generated self samples are
flagged. This limits confidence in fine guest-block rankings, without hiding the
unknown portion. JIT maps are zstd-compressed after analysis; a checked round trip
and both hashes are saved. Restore a map with `zstd -d file.jsonl.zst` when needed.

Guest counters record dispatch/cache/translation/poll events and interpreter
fallback reasons in a separate instrumented binary. Existing JIT counters supply
native steps, x87 helper steps, compilation counts, arena recycles and end-of-window
occupancy. Derived work includes steps/batch, dispatches/batch, batches/frame and
the fraction of batches ending in an x87 helper. These are work counts, not CPU
shares. Opcode frequency is not an optimization priority by itself.

## Decide whether a change can reach 5–10x

First locate costs that can change together. A helper boundary can cause generated
prologue/epilogue work, dispatch, state commits and device/clock polling; treating
only the helper's arithmetic as its cost misses those consequences. Conversely,
do not assign all dispatch cost to one instruction family from counts alone.

Use exclusive sampled shares and dynamic boundary counts to choose a structural
experiment, then test it with alternating uninstrumented 50-frame runs against
the frozen baseline. Amdahl's law bounds an isolated improvement: speedup is
`1 / ((1 - f) + f / s)`. Even eliminating 50% of time gives only 2x; 5–10x requires
changing at least 80–90% of today's cost. Keep correctness/coverage gates separate
from speed scores and report remaining fallbacks rather than rounding to 100%.
