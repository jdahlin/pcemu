# Host profiling after native batching

Three independent headless restored Excel recalculation tasks run under macOS
`sample` with the ordinary 16 MiB native-batch executable. Every expected result
pixel gate passes. This is a new post-change collection, not a transfer of the
previous enlarged-arena diagnostic's percentages.

After excluding 6,196 named attachment-barrier samples, the task contains
4,249 / 4,232 / 4,290 main-thread samples (12,771 total). Exclusive categories are:

| Work | Share of task samples |
| --- | ---: |
| JIT dispatch/cache/validation | 44.1% |
| Generated code and native helpers | 30.7% |
| Interpreter/MMU/faults | 11.5% |
| Device polling and interrupt delivery | 4.5% |
| Capture/video/control | 4.3% |
| Main loop/other | 2.7% |
| Compilation | 1.7% |
| Disk/compression | 0.5% |

These are sampled wall-stack shares, not exact CPU durations. Restore and named
profiler attachment waits are outside the task; capture and shutdown margins
remain. Relative shares change when another cost is removed: 44.1% does not mean
validation itself became proportionally slower. Uninstrumented paired speed
results remain in [native batching](native-batching.md).

`Jit::run()` has 4,876 self samples (38.2%); `runBatch()` adds 695 (5.4%). The
largest source site is `jit.cpp:89` at 936 self samples (7.3%), followed by line
99 at 2.9% and line 87 at 2.3%. These are optimized source locations spanning
fetch/metadata validation, not exact timings of one field or helper. A portion
of batch samples maps to line zero and cannot be assigned to a particular source
statement.

The evidence supports testing inlining of the within-batch dispatch call before
changing any validation invariant. Reusing validated CPU context inside a native
batch is another candidate, but requires proof that no native operation/helper
changes the assumptions, and still must validate the next cached block's context,
code generation and current fetch permissions. No guard is removed by this
profiling change.

The analyzer now recognizes `runBatch` as dispatch ancestry, keeping native
helpers/generated frames in execution and interpreter descendants separate.
Thirteen analyzer tests pass. All three raw traces have verified zstd round trips.
No new guest image or Office fixture is made; successful task children are removed.

[Build/source identity, complete per-run categories/sites/functions, task results
and compressed raw traces](measurements/post-batch-host-profile-2026-10-26.json).

The subsequent [standalone inlining experiment](batch-inline-evaluation.md)
verifies removal of that call, but mixed workload results do not justify keeping
the compiler hint alone. Production guards and code remain unchanged by it.
