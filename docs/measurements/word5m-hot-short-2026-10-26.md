# Word 97: hot short-fragment native coverage

The coverage-first follow-up improves the same 4.8 MB Word document launch.
Native share of emulator instruction counts rises from **87.3% to 90.8%**.
The frozen candidate uses **7.4% less median process CPU than f4abcad** and
**11.2% less than the original 57ce863 baseline** in an independent rotating
comparison. This is a new matched comparison, not a comparison with old timing
numbers from a quieter host session.

## Profile and coverage target

The preceding Instruments profile placed interpreter/MMU work first at 30.6%,
with dispatch/cache validation at 23.4%. MOV alone accounted for approximately
10% of interpreted attempts. Additional diagnostic counters established that
3,903,208 negative dispatches were fully supported short fragments deliberately
rejected by the admission policy; 1,864,269 of these included memory operations.

The change adds no new instruction semantics. It changes admission of existing
lowerings: an initially rejected supported fragment is reconsidered after
**64 further executions** without code/mapping invalidation. The ordinary
8-entry warmup is unchanged. Unsupported instructions keep their negative-cache
entries and are not repeatedly decoded. Short fragments are decoded afresh on
promotion, so the current guest bytes and RAM allocation are captured. Code
invalidation resets their heat. The marker fits in existing padding: cached
block size remains 88 bytes on this ARM64 build.

The normal build pays no diagnostic-counter overhead. Profile builds append
`shortFragmentHit`, `shortMemoryFragmentHit` and `shortPromote` events. The
counter-only reference has the marker and rejection classification but no
second-tier promotion, so it isolates the affected interpreted work.

## Diagnostic results

| Counter | Before promotion policy | Candidate |
| --- | ---: | ---: |
| Interpreted attempts | 12,536,756 | 9,024,620 |
| Negative cache hits | 9,600,745 | 6,096,114 |
| Rejected supported short-fragment hits | 3,903,208 | 411,664 |
| Rejected supported short memory-fragment hits | 1,864,269 | 215,920 |
| Device polls | 13,026,363 | 9,519,226 |

The candidate performs 3,103 second-tier promotions. Guest instruction totals
are similar (98.75 versus 98.44 million). Interpreted attempts fall by 28%;
short-fragment hits fall by 89%. Interpreted MOV r,rm attempts fall from
1,290,399 to 551,487. These are separate instrumented mechanism diagnostics,
not timing scores. Attempts include faults and REP host-batch resumptions.

The post-change Instruments recording has 2,025 main-thread task samples:

| Category | Share |
| --- | ---: |
| Dispatch/cache validation | 25.7% |
| Interpreter/MMU/faults | 23.5% |
| Generated code/helpers | 21.1% |
| Compilation | 12.1% |
| Capture/video/control | 9.8% |
| Device polling/interrupts | 5.4% |

These are sampled shares, not isolated function durations. No instrumented
run contributes to the performance comparison. Raw traces, lifetime-aware JIT
maps and full guest IP samples remain locally under `build/word-next/`.

## Original-baseline performance gate

All builds use the same immutable `build/snapshots/word5m-ready` checkpoint,
4,807,680-byte document, and unchanged original
[word5m-m1-2026-10-26.task.json](word5m-m1-2026-10-26.task.json).
Document SHA-256 is
`111edaf8471ba04a50c770fc5c5366b82013ec14558bc670633402c2b6fe5f7f`.
Readiness is still the rendered first-page cover title. It is not completion of
background pagination of every page. Direct linking is disabled in all scores.

Six exploratory rotating rounds were followed by a frozen-source rebuild and
**eight independent holdout rounds**. Each holdout rotates four labels:
original-A, original-B, previous Word build f4abcad, and candidate. Both original
labels use the identical rebuilt 57ce863 executable. All 32 independent restores
reach the expected pixels and exit cleanly; no failed or discarded scores.
This experiment launches no overlapping builds/profilers/benchmarks during the
timing runs. Normal host background activity remains.

| Build/label | Median process CPU | Median response | Native instruction share |
| --- | ---: | ---: | ---: |
| Original-A, 57ce863 | 2315.8 ms | 2433.8 ms | 85.7% |
| Original-B, identical executable | 2300.9 ms | 2380.7 ms | 85.6% |
| Previous f4abcad | 2216.2 ms | 2287.5 ms | 87.3% |
| Hot short fragments | 2051.6 ms | 2120.7 ms | 90.8% |

The pooled original CPU median is 2309.3 ms: the candidate median is 11.2%
lower. Versus f4abcad's median, it is 7.4% lower. The candidate beats both
same-round original controls and f4abcad in all eight rounds. Same-round
reductions versus the mean of original A/B controls range from 9.3% to 18.9%;
versus f4abcad, from 6.0% to 11.8%. All samples, including large A/A deviations,
are retained. Median absolute A/A difference is 1.25%; the largest is 9.77%.
Use the medians and complete paired data rather than attributing the unusually
large baseline samples to the implementation.

Process user+system CPU includes control, capture/video and device work, and
excludes restore/shutdown, blocked I/O and descheduled time. The comparison
measures the complete Word task, including all compilation costs. The candidate
adds roughly 3,000 compiled blocks rather than eagerly compiling every short
fragment. The descriptive paired bootstrap in the JSON describes this
host/session only. These results do not establish performance on every workload.

## Correctness and provenance

The frozen Release build passes core, bounded-batch and cached-batch suites.
The latter passes 176,950 checks. New tests cover register and memory short
fragments with paging on/off and direct links on/off, cold rejection, eventual
single promotion, poisoned old RAM before promotion, code changes resetting
heat, unsupported REP staying interpreted, and exact page-fault replay after
promotion. Additional PIT tests execute the promotion while IRQs are active,
with five initial phases and batched/unbatched execution, links off/on. They
require actual promoted execution, no early interrupt, latency at most 127
instructions, preserved registers/stack, and exact ISR/EOI accounting. The
bounded-batch suite passes 107,835 checks.

Eight host-profile accounting, two Instruments accounting and four debug-map
Python tests pass. The candidate was built from a clean f4abcad archive plus
only this change, excluding unrelated concurrent application/ARM edits. The
frozen executable matches the exploratory candidate byte-for-byte. Added test
coverage and the non-ARM unused-parameter fix do not change its executable hash.

- Original executable SHA-256:
  `1f2f4938d94f82cc9dc2852d00cd13b58d307a3fc1e69ef85c31d74772b57562`.
- Previous f4abcad executable SHA-256:
  `9db740e0d427c98b288824af05ef40d12e616e334f9396d576dfc13c3262ddbf`.
- Candidate executable SHA-256:
  `8e3031397f135d9004f2f280f1371e79bbdabb58f39336ed98e9af07ffd3c745`.

[Full timing records, source fingerprints, counters and test logs](word5m-hot-short-2026-10-26.json).

```sh
python3 tools/bench_snapshot_task.py build/snapshots/word5m-ready \
  --task docs/measurements/word5m-m1-2026-10-26.task.json \
  --emulator build/word-next/final/pcemu --repeat 1 \
  --restore-timeout 30 --output build/word5m-hot-short-recheck

ctest --test-dir build/word-next/final \
  -R '^(tests|jit-batch-tests|jit-cached-batch-tests)$' --output-on-failure
```

Run checks separately from timing. To reproduce the comparison, rotate eight
rounds of the four labels above and give each invocation a fresh output path.
Do not mix historical timing scores with measurements from a new host session.

## Coverage-first next steps

Dispatch is now the largest sampled category. That does not imply the next
change should be a dispatch micro-optimization: covering missing instructions
can join larger native fragments and remove dispatch boundaries too. Remaining
interpreted gaps include immediate stores (C6/C7, approximately 700,000 attempts),
memory ALU operations and memory bit tests. Prioritize hot unsupported code
and native-block breakers; do not chase 100% ISA coverage regardless of use.
Measure coverage/correctness while implementing those cases, and retain the
complete Word CPU comparison as the acceptance gate at each useful checkpoint.
