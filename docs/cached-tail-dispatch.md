# Cached native dispatch within a batch

`runBatch()` now enters through the normal dispatcher and then tries a small
cached-only native loop. A miss, stale metadata or fetch problem returns to the
normal dispatcher; compilation and interpreter replay stay outside the loop.
The helper checks context, RAM/code-buffer identity, stack/watch assumptions,
code generation and paged fetch permissions. It rechecks generation after a
fetch walk, since page-table accessed-bit writes can themselves modify code.

Each native function returns before the next target is selected. Metadata and
helper data cannot be replaced by compilation while these calls execute. Native
instruction prefixes commit before any interpreted fault/device replay. A zero
return without replay stops the batch. Logical dispatch counts and the existing
soft 64 / maximum 127 instruction limit preserve interrupt and control cadence.

CPU layout, generated instruction lowering, the single-block `run()` API and
snapshot format are unchanged. This is a cached C++ dispatch shortcut, not a
claim that blocks now branch directly to one another in native code.

## Measured behavior

Ratios below are baseline/candidate; above one favors the new controller.
Office process CPU includes guest/control/video work; restore is separate.

| Build and workload | Completed pairs | Median CPU ratio | Median response ratio |
| --- | ---: | ---: | ---: |
| Frozen same-source `-O3`, Excel calculation | 6 | 1.0343 | 1.0269 |
| Frozen same-source `-O3`, Word warm launch | 6 | 1.0014 | 0.9952 |
| Integrated default Debug `-O1`, Excel | 4 | 1.0354 | 1.0212 |
| Integrated default Debug `-O1`, Word | 3 | 1.0516 | 1.0632 |

All six O3 Excel pairs favor the controller, about 3.3% less CPU time at the
median. O3 Word is near baseline. Debug has substantial variation, including
pairs favoring baseline; the smaller Debug collection is supporting evidence,
not a formal confidence estimate. Two incomplete baseline observations are
excluded: a restore deadline expired before its first matching frame, and a
five-second shutdown wait expired after task completion. The original guests
were subsequently verified and stopped normally, and their owned children
removed. No incomplete sample is substituted into a pair.

Three paired RAM-only pointer-chase runs per dispatch API show roughly 1.11–1.20
wall-time speed ratios for the small batched working sets. The 8 MiB flat and
paged ratios are 0.995 and 1.002, respectively. Single-block ratios span
0.994–1.036. These are shared-host elapsed intervals, not isolated RAM latency.
No guest disk is needed. This does not establish another twofold useful-work gain.

An earlier inline `runBatch()` shortcut improved Excel but lost all three Word
pairs. Its results and exact patch are preserved alongside the retained helper
variant; it is not enabled. The helper avoids putting the entire cached path
into the outer batch loop.

## Correctness and acceptance

- 354,948 core checks, 3,500 existing batch/timer checks and 71,249 new cached
  batch checks pass. The latter compare multi-block register/memory cycles with
  independent interpretation and ordinary single-block execution, including
  paging, widths, warmup and block modes; 4,440 multi-dispatch batches occur.
- Focused tests cover partial page faults, exact MMIO callback count and clock,
  self-modifying code and fetch remapping through a colliding data alias.
  The private context-mutation suite also passes 759 checks.
- One frozen integrated executable boots DOS and passes all three reset paths;
  Windows 3.11 saves and reads back exactly 25 bytes; installed OSR2 and rtts
  run Calculator/Notepad, restart and read back exactly 38 saved bytes.
- Fresh OSR2 Setup completes unattended in the original process, including
  first-boot configuration, three resets, application checks, actual exit 0,
  normal quit trailer and stopped-guest exact 38-byte readback. Its compressed
  external child is removed after success. Long elapsed time includes intervals
  with little process CPU consumption and is not a startup score.

```sh
./tools/build.sh tests jit-batch-tests jit-cached-batch-tests
ctest --test-dir build -R '^(tests|jit-batch-tests|jit-cached-batch-tests)$' --output-on-failure
```

[Exact build/source identity, all comparisons, acceptance results and compressed driver evidence](measurements/cached-tail-2026-10-26.json).
The tested integrated build includes previously existing unrelated working-tree
edits; source and executable hashes identify it. Existing external checkpoints
and small compressed children are reused; successful children are removed.
