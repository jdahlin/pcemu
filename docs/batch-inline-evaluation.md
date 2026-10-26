# Within-batch dispatch inlining: standalone change not retained

The [post-batch profile](post-batch-profiling.md) attributes 5.4% of main-thread
self samples to `runBatch`, including loop bookkeeping and its call boundary.
A private matched-source candidate asks Clang to inline only the call to `run`
inside that loop. No validation, instruction budget or guest lowering changes.

The candidate uses Clang's [statement-level always_inline attribute](https://clang.llvm.org/docs/AttributeReference.html#always-inline-force-inline)
on an assignment expression, guarded by `__clang__`. Host disassembly verifies
one `bl Jit::run` in the baseline batch method and none in the candidate. The
batch body expands from 42 to 465 emitted host instructions; the standalone run
method remains available. The source difference and complete function disassembly
are preserved in the measurement record.

Three alternating headless pairs per existing task all pass their expected
pixels. Ratios are baseline/candidate, so above one favors inlining:

| Workload | Paired process CPU ratios | Median CPU ratio | Median response ratio |
| --- | --- | ---: | ---: |
| Excel calculation | 1.025, 1.040, 1.030 | 1.030 | 1.025 |
| Word warm launch | 0.966, 0.978, 1.003 | 0.978 | 0.992 |

Excel's modest gain is consistent across these pairs; Word is mixed and slightly
unfavorable at the median. Shared-host variation and three pairs do not establish
an intrinsic Word slowdown or its cause. The wrapper's sampled share was not
entirely removable call cost, so 5.4% was never a promised gain.

Keep production unchanged for this standalone hint. Inlining may still be useful
within a future measured dispatch optimization, but this trial does not establish
a broad improvement. In particular, none of the guard checks were removed.
No extra CPU/full-OS gate is claimed because the candidate is not retained.
No new guest or Office fixture is created.

[Exact source/build/binary identity, disassembly, drivers and every pair](measurements/batch-inline-2026-10-26.json).
The next substantive candidate is amortizing invariant CPU-context validation
within a bounded native batch, while proving cached-block identity and preserving
current code-generation/fetch checks. It is not implemented by this experiment.
