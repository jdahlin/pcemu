# Word 97: profile-guided 4.8 MB document loading

Word now shows a modest application-level improvement against the original
baseline: **about 2% less process CPU**, with unchanged default execution flags.
This result is for Word loading the document, not the earlier Excel task.
Direct linking remains disabled in every performance run here.

## Exact workload and baseline

The file is the same 4,807,680-byte binary DOC, SHA-256
`111edaf8471ba04a50c770fc5c5366b82013ec14558bc670633402c2b6fe5f7f`.
The reference is rebuilt from original commit `57ce863`, rather than compared
with old timing numbers. Both variants restore independently from the same
immutable Windows 95 checkpoint and launch `winword C:\BENCH5M.DOC`.

The original local checkpoint had been removed. The existing recovered Word
disk was flattened to local compressed QCOW2 with full byte comparison, and the
same document was staged into a private child with full FAT readback. An
original-baseline headless guest booted that child and saved the new checkpoint
before opening Word. No Office installation or raw disk staging was performed.
The usable checkpoint is `build/snapshots/word5m-ready`, backed by the local
`build/snapshots/word-local/disk.raw`; its writable snapshot child is 11 MiB.

Both the before and after reference pixel strings match the original task
byte-for-byte. Final timing uses the original committed
[word5m-m1-2026-10-26.task.json](word5m-m1-2026-10-26.task.json), including its
unchanged task hash. Readiness means the same first-page cover title has
rendered, not completion of background pagination of every page. The new
checkpoint's guest OS state differs from the old removed checkpoint; all
comparisons therefore rerun the original baseline on this checkpoint.

## Instruments evidence and changes

A fresh original-baseline Time Profiler recording contains 2,081 main-thread
samples inside the verified task interval:

| Sample category | Share |
| --- | ---: |
| Interpreter/MMU/faults | 31.0% |
| JIT dispatch/cache validation | 23.3% |
| Generated code/helpers | 15.9% |
| Compilation | 11.7% |
| Capture/video/control | 9.7% |

`CPU::physical` alone accounts for 5.7% of sampled self time, and `Jit::run`
for 12.8%. Fetch specialization, selective translation inlining, compilation
threshold changes, and alternate block modes were explored; none consistently
beat the original baseline. The translation and threshold changes are not
retained. All exploratory comparisons are preserved in the JSON report.

Guest counters then identified a concrete lowering gap: LODSB/LODSW/LODSD
accounted for 5.3% of interpreted attempts. The final changes are:

- Lower non-REP LODS and STOS through the existing guarded native RAM path.
  SI/ESI or DI/EDI changes only after successful access. DF, operand/address
  widths, segment overrides, MMIO replay and fault restart semantics remain
  precise. STOS continues to use ES regardless of a source-segment override.
- Lower CLD/STD so direction changes do not break otherwise native fragments.
  They preserve arithmetic flags, including pending native flag calculations.
- Admit hot short string-operation fragments rather than rejecting them as
  cheap fragments. REP forms retain the interpreter's existing count and
  interruptible scheduling behavior.
- Move native-only metadata behind a single owner, reducing each cached block
  from 104 to 88 bytes (the original baseline was 96). Negative entries and
  register-only unlinked blocks allocate no native metadata; operand vectors
  reserve the actual memory-operation count rather than the entire IR length.
- Specialize cached dispatch for linking on/off, removing linked-return
  accounting from the default path. The per-instruction dispatcher remains a
  single function. Profile categorization recognizes specialized dispatch names.

The integrated package is measured below; its individual changes should not
be assigned independent speedup percentages. The metadata changes alone did
not establish a win over the original baseline.

Separate diagnostic guest-counter runs reduced interpreted attempts from
14,134,208 to 12,553,515, despite guest instruction totals increasing slightly
from 98.11 to 98.59 million. Negative-cache hits fell from 11,291,854 to
9,612,016. Interpreted LODSD attempts fell from 322,303 to 3,651; LODSB from
425,864 to 47,291; STOSD from 426,693 to 52,027. The counter reference precedes
string lowering but already contains the metadata experiments, so these are
mechanism diagnostics, not original-baseline speed scores.

A final Instruments trace contains 1,959 task samples: interpreter/MMU 30.6%,
dispatch 23.4%, compilation 11.3%. Similar percentage shares do not imply
unchanged absolute cost. Neither trace nor guest-counter timing is used as a
performance score. Raw traces/maps remain under `build/word-optimize/`.

## Independent holdout comparison

After six exploratory rotating rounds favored the string-operation package,
the sources were frozen and rebuilt separately. The new executable matches
the exploratory candidate byte-for-byte. Eight new rotating rounds compare
original-A, original-B and candidate, where both original labels point to the
same executable. Every run is an independent restore, headless and unprofiled.
This experiment launches no overlapping benchmarks, builds or profilers in
the timing runs, and has no failed or discarded samples. Normal host background
activity remains; the rotating A/A labels measure its observed variability.

| Build | Runs | Median process CPU | Median response |
| --- | ---: | ---: | ---: |
| Original-A | 8 | 1968.0 ms | 2028.3 ms |
| Original-B, identical executable | 8 | 1962.1 ms | 2016.3 ms |
| Candidate | 8 | 1927.7 ms | 1983.6 ms |

The pooled original CPU median is 1965.8 ms: candidate median CPU is 1.9% lower.
Mean same-round CPU reduction versus the mean of A/B controls is **2.0%**;
all eight rounds favor the candidate (0.6–2.9%). Candidate also beats each
individual same-round control. Median absolute A/A difference is 1.18%, so
this is a small gain, not a large change in emulator performance. A descriptive
paired bootstrap interval of the mean is 1.55–2.40%; its method and seed are in
the JSON. It describes this host/session, not performance on other computers.

Median guest instruction counts are approximately 96.8–96.9 million in all
three groups. Capture/control work is included in process user+system CPU;
restore, shutdown, descheduled time and blocked I/O are excluded. Response
latency also improves, but remains sensitive to host scheduling and polling.
All 24 runs reach the original expected pixels and exit cleanly.

Original executable SHA-256:
`1f2f4938d94f82cc9dc2852d00cd13b58d307a3fc1e69ef85c31d74772b57562`.
Frozen candidate SHA-256:
`9db740e0d427c98b288824af05ef40d12e616e334f9396d576dfc13c3262ddbf`.
The rebuilt original has a different file hash from earlier sessions; source
revision, current hashes and individual runs are recorded rather than mixing
historical scores. See [the full report](word5m-string-ops-2026-10-26.json).

## Correctness and reproduction

The frozen Release build passes core, bounded-batch and cached-batch suites;
the latter passes 154,224 checks. New interpreter comparisons cover all string
operand/address widths, both direction values, paging, FS source override,
STOS ES selection, index wrapping, direction changes within a native block,
hot single-instruction admission, REP fallback, MMIO exactly-once replay,
self-modifying STOS and exact read/write fault prefixes. Existing invalidation,
metadata replacement, arena recycling and timer interrupt tests also pass.
Eight host-profile accounting, two Instruments accounting and four debug-map
Python tests pass.

With the retained local checkpoint, rerun one candidate measurement as follows:

```sh
python3 tools/bench_snapshot_task.py build/snapshots/word5m-ready \
  --task docs/measurements/word5m-m1-2026-10-26.task.json \
  --emulator build/word-optimize/final/pcemu --repeat 1 \
  --restore-timeout 30 --output build/word5m-recheck
```

For comparison, rotate eight rounds of two labels for the rebuilt original
executable and one candidate label, giving every invocation a fresh output
path. Keep all results. Build/check separately from timing runs:

```sh
ctest --test-dir build/word-optimize/final \
  -R '^(tests|jit-batch-tests|jit-cached-batch-tests)$' --output-on-failure
```

The remaining largest cost is still interpreter/MMU work. The counters point
to short memory/ALU fragments and memory bit tests for subsequent investigation;
the result does not justify enabling direct linking globally.
