# Guarded cross-block bridge prototype

A private prototype continues from a normal native exit into another compiled
block, bounded by the existing batch budget. Its C++ resolver accepts only a
matching fast-cache entry with current context, RAM identity, code generation
and fetch mapping. A fetch walk can itself modify code through page-table
accessed-bit writes, so generation is checked again afterward. The resolver
never compiles, interprets or invokes a device while native frames are live.
Early replay exits return the accumulated prefix before ordinary interpretation.

Calls read the current function from the stable block metadata node. They do
not retain old function addresses or helper pointers across metadata replacement.
The cache cannot clear while this resolver runs. Transient budget/dispatch bytes
fit existing CPU padding: CPU size and probed existing offsets are unchanged.
The public single-block API keeps its ordinary behavior. Aggregated instruction
counts and logical dispatch counts preserve the batch timer/control schedule;
exceptional exits also clear the transient budget.

Checks pass:

- 77,961 cross-block checks, with 4,256 batches confirmed to use links. Both
  register and memory cycles compare against independent interpretation and
  ordinary single-block execution across paging, widths, block modes and warmup.
- 1,084 focused memory checks including page fault after completed iterations,
  exact MMIO callback count/clock, self-modifying code and code PTE replacement.
- 3,500 batch/timer checks with the same modeled delivery points.
- 354,948 core checks and 759 context-mutation checks.

This implementation is **rejected as a default**. All three alternating paired
runs favor baseline for both tasks. Median baseline/candidate process CPU ratios
are 0.912 for loaded Excel calculation and 0.972 for Word warm launch. These
are uninstrumented headless comparisons with the frozen native-batch executable;
all expected screens pass. Successful disposable children are removed.

A separate diagnostic confirms the bridge is used, rather than simply missing
its targets: 28,543,791 ordinary native roots lead to 310,941,122 linked calls.
That is 91.59% of native calls bypassing ordinary dispatcher entry, with a mean
11.89 native calls per root. Histogram totals exactly match both counters and
the existing 339,484,913 NativeCall events. This still traverses a guarded C++
resolver and nested frames; those remain possible costs, not measured isolated
causes. Diagnostic timings are not compared with normal timings.

The next controller must reduce repeated resolver/frame work while retaining
these precise replay, lifetime and scheduling checks. This result does not
establish that direct native block linking cannot help. Production runtime code
is unchanged, and no installer acceptance claim is made for the private source.

[Exact patches, test sources and outputs, build commands, all paired results and diagnostic accounting](measurements/native-chain-bridge-2026-10-26.json).
