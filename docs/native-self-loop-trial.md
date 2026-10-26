# Bounded native self-loop trial

A private prototype repeats conditional backedges returning to the same native
block, keeping guest registers cached across iterations. It uses a second entry
point; the public single-block API retains its original behavior. Packed return
values distinguish accumulated guest instructions from logical block dispatches,
preserving the existing batch budget and control-pump cadence. Memory loops
check fetch translation and code generation before repeating. Partial exits
commit earlier iterations before interpreter fault/device replay.

Correctness checks pass:

- 232,960 register-loop checks against both independent interpretation and the
  ordinary single-block API, covering modes, paging, widths, flags and warmup.
  Actual native calls fall from 260,656 logical dispatches to 25,680 frames.
- 1,084 memory-loop checks including flat/paged loads, self-modifying code,
  MMIO exactly-once replay with the correct instruction clock, a page fault
  after eight completed iterations, and a PTE remap through a colliding data
  alias that leaves the old physical code generation unchanged.
- 3,500 existing batch/timer checks, with the same modeled delivery points.
- 759 context mutation checks and 354,948 core checks.

Six alternating uninstrumented headless pairs per task do **not** support
retaining this implementation. Median baseline/candidate process CPU ratios
are 1.014 for loaded Excel recalculation and 0.987 for warm Word launch.
Excel ratios span 0.767–1.154; Word spans 0.908–1.149. The second three-pair
collection reverses the apparent Word improvement in the first collection.
Response ratios are similarly neutral: 1.013 and 0.980. The shared host adds
substantial variation; these results establish no dependable application gain.

Production source and executable remain unchanged. Native loop repetition is
still a possible direction, but this emitter adds entry/footer code, memory
fetch guards and cache pressure. These are hypotheses for a more selective or
cheaper design, not costs isolated by the current measurements. A successful
RAM loop is insufficient evidence to ship a global policy.

[Exact patches, sources, commands, test outputs and all paired task results](measurements/native-self-loop-trial-2026-10-26.json)
are preserved. Existing external snapshots were reused; successful disposable
children were removed by the harness. No Office or OS disk was recreated, and
no new installer acceptance claim is made for this rejected prototype.

## Register-only isolation

A separate variant excludes all memory operations from repetition, eliminating
its per-iteration fetch helper. Three alternating paired runs still show no
consistent improvement: Excel median CPU ratio 0.985, Word 0.978. The existing
3,500 batch/timer and 232,960 focused register checks pass. This variant is also
rejected as a default. These measurements do not identify memory revalidation
as the sole reason the broader prototype failed; emitter size, loop frequency,
entry/footer overhead and cache effects remain possible contributors.

[Register-only patches, commands and paired results](measurements/native-register-loop-trial-2026-10-26.json).
