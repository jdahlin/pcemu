# CPU-context reuse prototype: not retained

The [post-batch profiles](post-batch-profiling.md) identify dispatch/validation as
the largest remaining category. This experiment captures invariant CPU context
lazily after a batch's first purely native block, then permits matching cached
blocks to reuse that validation. Production is unchanged.

## Proposed invariant and guards

The signature contains execution identity, CR0/CR3, CS base, VM mode, A20, watch
callback presence, stack address width, RAM pointer/size and code-bitmap pointer.
Native operations and continuing plain-RAM helpers cannot change those fields;
interpreter/I/O/replay boundaries stop the batch. The first block uses the full
standalone path. Signature changes advance a context identifier and native
blocks acquire its stamp only after their original guards pass.

Current instruction-fetch translation and permission checks, physical code-page
identity, generation counters, interrupt-shadow/TF checks and the existing
64-instruction soft budget remain. The prototype uses shared template dispatch
bodies, so source checks are not independently duplicated. It adds eight bytes
of metadata per block and changes the Jit object's layout and guard ordering.

## Evidence

The prototype passes the existing 3,500 batch/timer checks and 354,948 core
checks. Another 759 experimental checks compare full reference CPU/RAM state
and conservative guard rebuilding while mutating execution identity, CR0/CR3,
CS base, VM, A20, stack width, RAM buffers/bounds, watch callbacks and bitmap
storage. They also exercise code invalidation, current fetch remapping and
permissions, public single-block calls between batches and cache clearing.
The initial fetch fixture incorrectly enabled paging before constructing its
tables; that fixture ordering is corrected before the passing result. The
fixture source and initial failure are preserved with the final tests.

A separate diagnostic Excel run records 307.72M context reuses out of 339.46M
native calls (about 91%), with 26.68M lazy probes. It therefore exercises the
intended path. Instrumented timing is not treated as a speed comparison.

Two batches of three alternating headless pairs each preserve all expected task
pixels. The confirmation reverses the first execution order. Ratios above one
favor the candidate:

| Work | First CPU ratios | Confirmation CPU ratios | Six-pair median |
| --- | --- | --- | ---: |
| Excel calculation | 0.927, 1.157, 1.054 | 1.369, 1.140, 1.052 | 1.097 |
| Word warm launch | 0.934, 1.024, 1.041 | 0.944, 1.012, 1.015 | 1.014 |

Five of six Excel pairs favor the prototype; Word is roughly neutral. The wide
shared-host spread prevents claiming a stable application percentage.

A separate RAM-only control calls the public standalone `run()` without batching.
Three alternating 50M-instruction pairs per code/stack/paging case yield median
ratios of 0.946, 0.971, 0.985, 0.985, 0.986, 0.997, 0.996 and 1.001. Every
checksum/count passes, but most cases are slower. These controls do not isolate
whether metadata size, layout, ordering or generated host code causes the cost.

## Decision

Do not promote this design. The loaded Excel result is promising, but the
standalone path regresses and Word does not establish a broad gain. No extra
full OS boot/restart or fresh Setup gate is claimed for this non-retained
candidate. The proven production batching remains intact.

A future attempt should preserve the standalone path and metadata footprint,
or reuse local invariant values without persistent per-block stamps, before
another default change. The frozen mutation fixture supplies evidence for that
work; its rebuild-counter assertions are not added as permanent constraints on
other correct cache designs.

[Exact prototype patches, commands/binary identities, all twelve application
pairs, RAM controls, test source/results and compressed diagnostic counters](measurements/context-cache-prototype-2026-10-26.json).
No new Office or large VM fixture is made. Existing task children are removed
by the harness. Production has no new context stamp, template dispatcher or
profile events from this experiment.
