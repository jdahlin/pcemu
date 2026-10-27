# Quake batched JIT baseline — 2026-10-27

This is a development baseline for a 5–10x target. Every run uses the same
50-frame `demo1` prefix; no full-demo runs were used. Source commit `0ebca12`
was captured cleanly and built with `-O3 -DNDEBUG -g -gdwarf-4
-fno-omit-frame-pointer` on an Apple M1 Pro. Sources, binaries, guest-image hashes,
symbols and raw traces are frozen in `build/quake-jit-batched-baseline/`.

The benchmark and ordinary session share `runPcBatch`: `runBatch(64)` followed by
the same device polls and 50,000-step timer checkpoint. Quake uses eager
compilation, same-page jump joining, a 16 MiB arena and no experimental direct
links. The benchmark retains its documented wall-clock PIT source. Observation
points stop root and cached batches at the original timedemo boundaries.

## Uninstrumented speed

Ten independent timing runs, with sampling and guest counters disabled, give:

| Metric | Result |
| --- | ---: |
| Median FPS | 35.65 |
| Median wall time/frame | 28.050 ms |
| Median thread CPU time/frame | 27.736 ms |
| FPS range | 34.58–36.99 |
| FPS median absolute deviation | 0.685 |
| Coefficient of variation | 2.30% |
| 5x target | 178.25 FPS / 5.610 ms per frame |
| 10x target | 356.50 FPS / 2.805 ms per frame |

The initial five-run cohort drifted -5.19% from first to last. A separate five-run
confirmation with the same frozen executable had +0.69% drift and 1.70% CV. Both
cohorts are retained; the primary score includes all ten timings. These spreads
are not confidence intervals. Future comparisons should alternate baseline and
candidate runs to control drift rather than compare isolated best results.

## Where CPU samples go

Five independent Instruments collections of the same 50 frames contain 6,896
main-thread CPU samples inside the exact timed-frame windows. Exclusive shares:

| Work | CPU sample share |
| --- | ---: |
| Compiled x87 helpers | 23.0% |
| Dispatch, cache and fetch validation | 19.3% |
| Mapped generated code | 17.7% |
| Compilation and decoding | 16.2% |
| Host clock reads, principally PIT/runner | 13.3% |
| Unmapped generated PCs | 3.0% |
| Compiled string helpers | 2.9% |
| Polling/IRQ delivery excluding clocks above | 0.6% |
| Interpreter/faults | 0.03% |
| Remaining host/helper work | 3.9% |

Mapped generated prologues alone account for 8.06% of all main-thread samples;
host epilogues add 1.13%. Those are **subsets** of generated code, not additional
shares. Typed x87 helpers execute outside generated host frames and are classified
by explicit symbol ancestry, rather than being mistaken for interpreter work.

JIT maps identify PID, publication time and arena generation. Address reuse is
resolved by sample timestamp. The 210 unmapped generated self samples remain
explicitly unknown, including conservative publication/recycle uncertainty.
They limit fine guest-block rankings. Sample shares are estimates, not exact
function durations; instrumented FPS is excluded from the speed baseline.

## The structural bottleneck

Median uninstrumented work is approximately:

- 277,758 batch calls/frame, averaging only **6.27 guest steps/batch** against a
  nominal 64-step soft budget; 1.53 dispatches/batch.
- **94.17% of batches end in a compiled x87 helper.**
- 1,376 compiled blocks/frame and **four arena recycles per 50-frame run**.

Guest steps use the emulator's accounting, including bounded REP continuations;
they are not hardware-retired x86 instructions. Work counts do not establish CPU
cost by themselves, but they explain why a larger configured batch budget is
currently ineffective: the x87 helper boundary ends execution first.

Eliminating only the sampled x87 helper arithmetic's 23% share has an ideal
ceiling of about 1.30x. A 5–10x experiment must remove costs together. The first
candidate is lowering common x87 operations inside longer native blocks while
retaining CPU state across them. That can affect helper calls, dispatch,
prologues/epilogues, polling/clock reads and generated-code footprint. Its speedup
is unproven; correctness and paired 50-frame timings must establish it.

Arena sizing alone addresses only part of the measured compilation cost. Repeated
compilation reasons and dynamic block reuse need closer attribution before
claiming that all cache churn is avoidable. The remaining interpreter cost is
far too small to explain the requested speed gap.

## Evidence and reproduction

The [compact machine-readable record](quake-jit-batched-baseline-2026-10-27.json)
contains every timing value, configuration, source/binary identity, CPU categories,
region shares and limitations. Raw collection files are:

- `build/quake-jit-batched-baseline/report.json`: original five timings and all
  diagnostic-run metadata.
- `confirmation-report.json`: the five additional timings and pooled ten-run score.
- `host-analysis.json`, per-run `instruments.trace` and exported XML: exact-window
  CPU attribution.
- Per-run `jit-debug.jsonl.zst`: raw JIT maps, with verified decompression round trips
  and hashes in `manifest.json`.
- `guest-counters/guest-profile.json`: a separate counter run whose interval bounds
  match the frame window and whose interpreted attempts reconcile with total minus
  native steps (1,704 in the whole frame window, including DOS/CWSDPMI).

The original collection command was:

```sh
python3 tools/profile_quake.py --frames 50 --repeat 5 --host-repeat 5 \
  --timeout 240 --output build/quake-jit-batched-baseline
```

Confirmation used the same frozen `source/build/quake-bench` through the frozen
`source/tools/bench_quake.py`, with `--no-build --backend jit --measure-window
--frames 50`, one excluded warmup and five timing runs. All successful scratch
guest disks were removed. The default collector can gather ten timings directly
with `--repeat 10`; preserve its complete distribution and drift measurements.
