# Why the software-MMU prototype appeared slower in Word

There is confirmed unnecessary dispatch work, but the earlier approximately 6%
overall Word regression is not a stable estimate. Larger balanced comparisons
put the old prototype near the reference. Keep the prototype experimental;
do not attribute the entire earlier difference to TLB size or page walking.

## Confirmed overhead and isolated change

The prototype called `CPU::syncNativeTLB()` at the beginning of every
`Jit::run()`, including interpreted dispatches. Word launch executes roughly
31 million guest instructions, about 7.5 million outside native lowering.
Those dispatches do not consume cached host addresses but paid the context
check anyway. Loaded Excel stays in native batches more often.

A private variant moves synchronization immediately before the native function
call, after interpreter exits, compilation and context/fetch checks. Cached
native tails begin from this synchronized root and stop at interpreter/replay
boundaries. RAM pointer/size, A20 and execution-epoch validation still occurs
before using cached host addresses. Entry metadata and the 32 KiB table remain
unchanged, isolating check placement from cache capacity/layout.

Two separate diagnostic runs per variant count actual calls during the task:

| Median task counter | Old prototype | Native-entry-only check |
| --- | ---: | ---: |
| Context checks | 11,071,734.5 | 3,785,903 |
| Derived-cache fills | 892,115.5 | 898,397.5 |

About 7.3 million checks disappear, a 65.8% reduction. Counter builds are
instrumented separately; their timings are not used as performance scores.

## Host profiles

Three verified traces each for reference and old prototype yield 2,561 and
2,656 main-thread task stack samples. Ten-second named attachment barriers
are excluded. These are sampled wall stacks, not exact CPU durations:

| Category | Reference | Old prototype |
| --- | ---: | ---: |
| Interpreter/MMU/faults | 23.7% | 24.7% |
| Dispatch/cache/validation | 18.0% | 22.0% |
| Compilation | 10.9% | 8.6% |
| Generated code/native helpers | 10.5% | 9.7% |
| Disk/compression | 22.8% | 20.2% |

Word launch offers relatively little time in the native RAM fast path for the
new lookup to improve. Cache synchronization appears in 1.13% of old-prototype
samples; cache filling appears in 0.23%, and reference page walks in 0.41%.
This does not support page-table walking or filling as the dominant cause.
Higher dispatch share also cannot by itself prove an absolute duration increase.

Three later native-entry-only traces show six synchronization samples out of
3,680, consistent with fewer calls. This follow-up has substantially more
capture/control/IO samples and is not a matched CPU-speed comparison. All raw
traces are retained as byte-verified zstd files.

## Timing controls and limits of the original result

Six balanced, uninstrumented triplets compare reference, old prototype and
native-entry-only prototype on the same restored Word launch task. Every
expected rendered result passes. Median paired process-CPU ratios:

- Reference / old prototype: **1.0094**, near baseline rather than a stable 6% loss.
- Old / native-entry-only: **1.0122**, favoring the placement change by about
  1.2%, with four of six pairs favoring it. This is below observed A/A variation;
  no statistically established application speedup or full 6% recovery is claimed.

A separate six balanced triplets runs two labels pointing to the identical
reference executable alongside a layout-only control. That control has the
prototype's enlarged/aligned CPU object but the reference memory and dispatch
paths. A/A CPU ratios span **0.9635–1.0251**. The layout control's median ratio
against the geometric mean of the two references is **0.9913**. It does not
establish a large or dominant CPU-object/layout penalty, and is not a hardware
cache-miss measurement. No outliers are removed from either collection.

The short launch fixture includes control, framebuffer capture and polling.
The initial three-pair apparent regression includes substantial host system CPU;
it is not isolated guest execution cost. Captures themselves appear in the
profiles, and first-render detection admits varying trailing guest work.
Internal `executionWallMs` excludes capture/control work but includes host
preemption and guest disk waits; it must not be treated as process CPU time.
Longer loaded Word work is needed for broader application-performance claims.

One attempted initial profile missed its three-second attachment barrier and
is excluded. A later follow-up's initial restore spanned macOS idle sleep;
its task attachment occurred after wake. That whole collection is excluded
conservatively for its changed post-resume environment, not as evidence that
timed guest work ran asleep. The retained follow-up uses temporary
`caffeinate -i`; no persistent power setting changes. Initial profiles and all
uninstrumented comparisons completed before the recorded idle-sleep interval.
External-storage restore delays remain outside task timing. No media or large
VM fixture is recreated; successful compressed children are removed.

## Correctness and retained implementation

The placement variant passes 354,964 core checks, 2,900 snapshot/COW checks,
71,249 cached-batch checks (4,440 multi-dispatch batches), and 759 context
mutation checks, including RAM replacement and A20 transitions. Tests and
runtime sources are frozen for these legacy-storage prototypes; this is not
an acceptance run of unrelated concurrent storage changes. No new cold-boot
or fresh-Setup acceptance is claimed for this placement variant. Shipping
runtime remains unchanged.

[Evidence, exact source patch, binary/source identities and compressed traces](measurements/word-softmmu-investigation-2026-10-26.json)
separate profiles, diagnostic counts, uninstrumented comparisons and exclusions.
The fixed private executable is `build/softmmu-native-sync/candidate-pcemu`.
The check placement should be retained when integrating the MMU work; remaining
performance claims need longer useful work and continued control measurements.
