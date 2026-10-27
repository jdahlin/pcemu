# Quake native x87: 50-frame comparison

Implementation: `8305377`. Five alternating baseline/candidate pairs use frozen
optimized executables, the same 50-frame demo prefix, silent 320×200 output,
32 MiB guest RAM, and the production `runBatch(64)` polling path. The JIT arena
remains 16 MiB, eager compilation and same-page jump joining remain enabled,
and direct links remain disabled. Builds use `-O3 -DNDEBUG -g -gdwarf-4 -fno-omit-frame-pointer` on Apple M1 Pro.

| Metric | Requested baseline, rerun | Native x87 |
| --- | ---: | ---: |
| Median FPS | 39.66 | 101.88 |
| FPS range | 39.01–39.82 | 98.15–102.08 |
| FPS coefficient of variation | 0.83% | 1.63% |
| Median wall ms/frame | 25.216 | 9.816 |
| Median thread CPU ms/frame | 25.205 | 9.805 |

**Median paired speedup: 2.56×. The 5–10× target is not reached.**
Pair ratios are 2.574, 2.516, 2.559, 2.561, 2.561.
The requested baseline was previously recorded at 35.65 FPS; today's reruns
are faster, so the paired result is the primary comparison. The original target
was 178.25–356.50 FPS. Relative to today's baseline, 5× would require
198.29 FPS / 5.043 ms per frame.

## Structural effect

Medians per completed 50-frame interval:

| Metric | Baseline | Native x87 |
| --- | ---: | ---: |
| Decoded x87 helper operations | 13,078,765 | 26,078 |
| Batch calls | 13,897,707 | 1,406,632 |
| Dispatches | 21,350,319 | 9,364,686 |
| Compiled blocks | 68,928 | 32,313 |
| Arena recycles | 4 | 5 |

Helper operations fall **99.80%** and batches fall
9.88×. Approximate guest step units per batch
rise from 6.29 to 64.90;
64 is a soft budget and a completed block may overshoot it. Remaining typed x87
helpers cover uncommon forms and unsafe/cold operands. Bigger native blocks
reduce compilation count, but generated guards and prefix-commit stubs still
recycle the arena more often; fewer compiled blocks do not prove lower code
footprint. No cache-size change is included.

A separate strict coverage run passes with 91,030,919
counted application guest steps, **100% compiled coverage**, zero interpreted
steps and zero predecode interpreter calls in the exact timed window. DOS and
CWSDPMI are excluded. This is one observed coverage pass, not a claim that all
possible cold-fetch cases are resolved.

## Remaining CPU costs

Seven separate Instruments collections contain **3,794 main-thread CPU
samples** in the exact timed-frame intervals. Their instrumented FPS is excluded
from the speed result. Exclusive sampled shares:

| Work | Share |
| --- | ---: |
| Compilation and decoding | 35.5% |
| Generated code | 28.6% |
| Dispatch/cache/fetch validation | 14.1% |
| Unmapped generated code | 7.7% |
| Compiled string helpers | 7.5% |
| Host clock reads (PIT/runner) | 4.3% |
| Other host work | 1.2% |
| Other compiled runtime helpers | 0.6% |
| Compiled x87 helpers | 0.3% |
| Device polling and IRQ delivery | 0.2% |
| Compiled segment/interrupt/I-O helpers | 0.1% |
| Interpreter and guest faults | 0.0% |
| Diagnostic code-map bookkeeping | 0.0% |

Compilation/decoding and generated code now dominate. Eliminating the remaining
x87 helper arithmetic cannot produce another large gain. Executable footprint,
recompilation and the generated integer/memory/exit paths are the next areas
supported by these measurements. Unmapped generated PCs remain explicit;
these samples do not justify assigning all generated-code time to x87.

## Validation and reproducibility

737,753 native/decoded x87 checks pass, including differential exceptional-value,
TOP/RC, stack wrapping, ABI, cached-batch and precise side-exit cases. Relevant
cached batching/context, decoded boundaries, batch IRQ latency, flag liveness,
stack and far-transfer suites also pass, plus the Quake benchmark harness and
14 Python benchmark/profiling tests.

[Complete timings, hashes, counters, coverage and sample aggregates](quake-native-x87-2026-10-27.json)
retain every pair. Frozen sources, binaries, logs, traces and compressed,
round-trip-verified JIT maps are in `build/quake-native-x87-final/`. Source and
binary hashes, demo hash, execution configuration and unchanged backing-disk
hashes were checked before recording this result. All successful scratch
compressed overlays were removed. No full demo or large VM fixture was used.

The candidate also contains intervening CPU correctness fixes since the requested
baseline commit `0ebca12`; this is a comparison against that exact requested
baseline, not an isolated single-commit ablation. Shared-host variance and the
host-clock PIT also mean guest step counts differ. The useful-work gate is the
same completed 50-frame demo. Native lowering preserves the current binary64
x87 model and its documented precision/exception limitations.
