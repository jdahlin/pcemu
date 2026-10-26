# Batch lookup-key hoist: not retained

The [full local snapshot](local-context-evaluation.md) did not improve the tasks.
This narrower follow-up captures only CS identity's contribution to the lookup
key: the selector/operand-mode prefix and selector hash. Native-only execution
cannot change CS; interpreter/replay boundaries stop the batch. Every metadata,
code-generation and current fetch check still reads the live CPU as before.

A shared template emits standalone and within-batch code. The two-value state
is local to a batch; no Jit or block data members are added. The instruction
budget, polling granularity and debugger paths remain unchanged.

The existing 3,500 batch/timer checks and 759 experimental context mutation checks
pass. All expected task pixels also pass in three alternating headless pairs per
workload. Ratios are baseline/candidate:

| Task | Paired CPU ratios | Median |
| --- | --- | ---: |
| Excel calculation | 0.832, 0.960, 1.043 | 0.960 |
| Word warm launch | 1.016, 0.998, 0.868 | 0.998 |

Do not retain this version. Shared-host spread prevents isolating the cost of
key arithmetic, register pressure or host code layout; the measurements do not
justify a default change. No additional standalone RAM, full-OS smoke or fresh
Setup gate is run for a variant that does not pass its task-performance gate.
No new Office or large VM fixture is made. The proven production native batching
and its OS gates remain intact.

[Exact source/build/binary identity, test results, drivers and all six pairs](measurements/batch-key-prototype-2026-10-26.json).

These local-hoist experiments do not establish that every dispatcher operation
is intrinsically expensive. The next substantial direction is guarded native
block linking, which could avoid repeated C++ dispatch altogether. A first
prototype should restrict links to compatible CPU contexts and code pages,
retain current software-TLB/fetch checks and code generation, keep helper data
alive for every reachable emitted block, and stop before any interpreter/I/O/
replay boundary. Native budget and debugger observability must remain bounded.
That architecture is not implemented or claimed by this experiment.
