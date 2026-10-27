# Profiling Quake before optimization

Run the workload-specific collection with:

```sh
./run-quake.sh --profile --frames 50 --repeat 5
# An explicit new directory makes the evidence easy to keep and compare:
python3 tools/profile_quake.py --output build/quake-profile-before
```

This collects data for an optimization target; it does not assert that a 10x
speedup is available. This collection explicitly selects the interpreter.
The JIT now runs Quake (see [compatibility validation](quake.md)), and the
coverage runner has observation fences for single-block dispatch. This host
profiling workflow still rejects JIT collection: the production application uses
native batching, which needs equivalent exact-window observations before this
workflow can provide a representative JIT baseline.
Use the 50-frame prefix for iteration, then repeat milestones with `--frames 969`
and the full correctness suite. The prefix measures the opening scene rather
than the scene mix of the complete demo.

## Keep scores and explanations separate

Each session copies nonignored build inputs into an owned source directory.
Working-tree edits and untracked source files used by CMake are included; media,
VM disks and ignored output are not copied. The manifest records every source
hash, Git state, compiler version, flags, binary hashes, guest backing-chain
hashes, host information and load. A source change during capture fails rather
than silently mixing inputs. Run binaries and symbols remain in the session.

The build uses `Release`, `-O3 -DNDEBUG -g -gdwarf-4 -fno-omit-frame-pointer` in
its own build directory. It does not change the checkout's ordinary build type.
DWARF4 and an adjacent macOS dSYM avoid symbol-decoder problems with newer debug
formats. Keep the source/object files, binaries, dSYM and raw trace together.

The sequence is one excluded warmup, a baseline, native stack sampling, guest
counter profiling, then the remaining baselines. All runs cold-boot separate
small compressed overlays of the existing disk and use the same generated demo.
Successful scratch disks are removed. No raw disk or RAM snapshot is recreated.

Only `baseline-*` runs define speed. The report gives exact-window FPS, median,
median absolute deviation (MAD), range, coefficient of variation (CV), thread
CPU/wall ratios and first-to-last drift. Five runs are a starting point, not a
population confidence guarantee. Warmup, sampled and counter-instrumented FPS
are retained as diagnostics and excluded from baseline statistics. Baseline
binary/demo/frame identity is checked; altered backing chains fail collection.

## Measure the actual timed frames

Console messages do not identify the exact start of rendered timedemo work:
"Playing demo" precedes map loading. The profiling window instead uses two
validated, read-only instruction boundaries in the original DOS Quake 1.06:

- After EIP `00003ac3` stores `cls.td_starttime`, execution reaches `00003ac9`.
  `host_framecount` must equal `td_startframe + 1`: the warmup frame is excluded.
- At `CL_FinishTimeDemo` entry, EIP `00003f86`, capture stops before timing and
  result-printing work. The host-frame delta must equal Quake's final frame count.

The offline executable SHA-256 must be
`7cdb244122cc607cc6f162524bfcac60f6d11d8ca2ff07473d6cd962311161fb`.
The runner also validates mode, segment context and instruction bytes at both
boundaries. A different binary or unexpected frame sequence fails. It never
patches game instructions. The mapping follows the released
[CL_GetMessage and CL_FinishTimeDemo code](https://github.com/id-Software/Quake/blob/master/WinQuake/cl_demo.c)
and the checked original executable; it is not a generic source-symbol map.

The baseline hot loop adds two guest-IP comparisons per dispatch. CPU and wall
clocks are read at the boundaries, not for every measured instruction. This
small measurement mechanism is common to baseline and diagnostic runs; its cost
remains part of the measured runner and can appear in host stacks.

`window.json` records exact guest instruction bounds, host frame bounds,
monotonic wall time and executing-thread CPU time. The FPS used in the baseline
summary is frames divided by this high-resolution wall interval, rather than
Quake's rounded one-decimal console value. Original guest-reported FPS remains
available for cross-checking. Host time feeds the emulated PIT in all these
runs; it does not impose a playback/frame-rate limit.

## Native host stacks

The macOS `sample` tool attaches only after the guest reaches the timing-start
boundary. A named barrier holds guest execution until attachment is confirmed.
That wait is excluded from the measured window and subtracted from the guest
PIT clock, so it does not advance Quake time. The sampler exits when the runner
finishes; a brief attachment/exit tail can remain in the raw trace.

`host-analysis.json` selects the main thread and derives self counts from each
node minus its immediate children. Self counts conserve the thread total.
Inclusive counts deduplicate recursion along a path but still overlap between
functions: adding inclusive percentages is invalid. Sleep/wait leaf samples and
unresolved symbols are reported separately. The raw `sample.txt` and sampler
log are retained. Symbol decoder warnings and fewer than 1,000 non-idle samples
are flagged; use a longer demo if attribution is too sparse.

Stacks from an uninstrumented optimized binary are the primary evidence for
whole-run bottlenecks. The sampled run uses the exact same binary as the baseline,
but its time is not a performance score. `--no-host-sample` supports platforms
without macOS `sample`; the report explicitly warns that counters alone cannot
establish whole-process CPU attribution.

## Guest diagnostics

A separate `PCEMU_PROFILE` executable collects only inside the same frame window:

- Primary/extended opcode attempts, operand kinds, groups and dispatch reasons.
- Jittered guest instruction-pointer samples with CR3 and CS context.
- Existing sampled thread-CPU instruction cost estimates with clock-pair
  correction, fault counts and low-sample indicators.
- Exact decoded D8–DF opcode/ModR/M counts, so x87 forms such as FXCH, FLD,
  memory arithmetic and integer stores do not collapse into one broad bucket.

`guest-analysis.json` includes these rankings and the raw profile is retained.
The guest profile's start/stop instruction counters must exactly match its
validated window. Decode counts omit x87 instructions that fault with #NM before
ModR/M is fetched. Counts include restarted attempts and REP scheduling batches.
0F primary and extended counts overlap and must not be added together.

Instruction timing includes diagnostic instrumentation and samples only work
inside `CPU::step`; it excludes machine polling, dispatch and other host work.
Do not use its extrapolated percentages as whole-process CPU shares or compare
its FPS with baseline FPS. Use it to explain the native profile and design
small, falsifiable optimization experiments.

## Use the evidence to pursue 10x

Start with the main-thread native profile, then use guest opcodes and hot IPs to
explain which guest operations exercise those host paths. Distinguish interpreter,
memory/device, benchmark clock and diagnostic overhead. In particular, the
wall-clock PIT's polling cost belongs to this measured board configuration;
normal instruction-clock sessions have different timing overhead.

Optimize one measured bottleneck at a time. Freeze a new session with the same
frames, demo hash, backend, resolution, compiler flags and host. Compare baseline
medians and dispersion, not the best run or an instrumented score. Check the full
demo and guest correctness before claiming a general speedup. Amdahl's law means
speeding up a small fraction of execution cannot yield a 10x whole-run gain.

Tests cover boundary fingerprints/warmup/frame counts, x87 counter activation
and reset, sample-tree conservation/recursion, rejection of mixed or profiled
baseline scores, statistics, frozen input selection and concurrent-edit detection.
These run with `make test`; the guest capture itself stays an explicit local
integration run because it requires the prepared shareware/DOS disk.
