# Bounded native execution between device polls

The [host profiles](office-current-profiling.md) identify frequent board polls
and dispatch overhead as major loaded-Excel costs. The main loop now executes
a bounded sequence of native blocks before calling `Machine::poll`, while
retaining the existing JIT entry/code/MMU validation at every block.

## Execution boundaries

`Jit::runBatch` uses a soft instruction budget capped at 64. Each native block
already has at most 64 decoded guest instructions, so a batch completes at most
127 before returning. A zero budget does nothing. The result reports both
completed instructions and original dispatch count; the main loop uses dispatch
count to retain approximately the existing control/network/frontend pump cadence.
The headless instruction limit also constrains the requested budget.

Batching stops after any interpreter instruction, including warming/negative
fragments, unsupported I/O or state changes, guarded MMIO/fault replay, HLT and
interrupt-shadow/TF fallback. An interpreter contribution is detected by the
native-instruction counter differing from the returned total. No CPU fields or
native instruction encodings change. Code invalidation and permission checks
still occur in the unchanged `run()` at every entry.

The caller polls devices after every batch. Active breakpoints, trace output
and trace-tail observation retain single-block `run()` behavior. As with existing
native blocks, interrupt delivery is not cycle-accurate. The new maximum gap is
127 guest instructions rather than one block. A real PIT/PIC/EOI/IRET test across
five starting phases observes worst IRQ0 lateness of 58–61 instructions with
batching versus 2 without, while interrupt count, checksum and stack state match.
This is an explicit timing-granularity change, not a claim of identical IRQ points.

## Application measurements

Three alternating uninstrumented pairs use the previous inactive-breakpoint
candidate as baseline. Only the batch API and main-loop integration differ;
the arena remains 16 MiB. Every expected rendered task result passes.

| Workload | CPU ratios, baseline/candidate | Median CPU ratio | Median response ratio |
| --- | --- | ---: | ---: |
| Excel dependent calculation | 1.302, 1.314, 1.314 | 1.314 | 1.309 |
| Word warm launch | 1.085, 1.087, 1.063 | 1.085 | 1.074 |

All pairs favor batching. The median ratios correspond to about 24% less process
CPU for Excel and 8% for Word on these tasks. This is stronger consistency than
the preceding small experiments, but still three shared-host pairs per workload,
not a universal gain or the twofold useful-work target. Restore is separate;
control/capture/observation inside the response interval remains included.

A separate diagnostic Excel run executes about 1.404 billion guest instructions.
Device polls fall to 39.83M, versus 359.55M in the earlier native-PUSH diagnostic,
about 88.9% fewer. Dispatches remain about 359.7M, confirming that existing block
validation still runs. The comparison uses separate diagnostic runs and the
older guard baseline; they are not lockstep traces or timing comparisons.

## Correctness and storage

The current CMake suite passes 3,500 combined checks (995 CPU/batch checks and
2,505 real-board timer checks), spanning Basic/FollowJumps/Trace modes, code and
paging widths, warmup, budgets, precise faults, MMIO callback counts, CLI/HLT,
interrupt shadow/TF fallback and arena recycling. The 354,948-check core suite
passes, and the current `pcemu` target compiles.

Actual CLI checks pass DOS JIT/interpreter boots, a zero-instruction boot-entry
breakpoint and an exact three-instruction post-break trace. The frozen candidate
also passes Win3.11 Notepad/save/DOS readback and both rtts/installed OSR2
Calculator/clipboard/Notepad/restart/exact-file gates. Successful private disks
and orphan indexes are removed. No new Office fixture is made.

Fresh OSR2 Setup also passes with the same executable: 56 source files verify
on a compressed DOS child; one process completes all Setup/configuration stages,
three resets, application checks, normal exit status 0 and independent exact
38-byte readback. The 154,444,294-byte child and its index are removed. A
known-screen driver runs the wizard without intervention after launch; credentials
stay in the ignored local control transcript. See [fresh-install evidence](fresh-win95-validation.md#uninterrupted-run-with-bounded-native-batching-26-october).
The broader twofold useful-work goal remains active.

[Exact frozen patches, build/binary identity, all pairs, diagnostic counters,
CLI states, timer results and OS verification](measurements/native-batching-2026-10-26.json).
Full guest counters are retained once in a verified zstd artifact.

```sh
cmake --build build --target jit-batch-tests tests pcemu
ctest --test-dir build -R '^(jit-batch-tests|tests)$' --output-on-failure
```
