# TLB capacity trial

The new dependent-load benchmark exposes substantial capacity pressure with
an 8 MiB paged ring. A private same-source `-O3` prototype increases the direct
mapped TLB from 1,024 to 4,096 entries and changes the emitted ARM64 index mask
accordingly. Guest page size, permissions, dirty handling and invalidation
semantics remain the same. The default runtime is unchanged.

Three alternating RAM-only pairs at 20 million traversals show a median 1.57×
speedup for the 8 MiB paged case. Slow native memory-helper calls fall from
10,127,014 to 2,040. Smaller working sets show no clear gain; some flat controls
regress (8 MiB median ratio 0.906). Ratios are baseline/candidate; above one
favors the prototype. Host variation and fixed case order limit interpretation.

Six alternating headless restored Office pairs do not justify enabling the
larger table. Excel's median process CPU ratio is 1.032, with pair ratios from
0.951 to 1.364. Word's median is 0.913, with four of six pairs favoring baseline;
all three confirmation pairs favor baseline. All expected screens pass.
The larger table's host-cache footprint is a possible cost, not a proven
explanation. A dedicated memory stress win is insufficient for a global policy.

The frozen Office comparison source uses snapshot schema 4. Its private adapter
rehashes live translations from legacy 1,024-entry checkpoints and uses a
private `0x80000004` marker for enlarged-table round trips. It is not a proposed
production format: current production schema includes newer device state.
Existing Office checkpoints restore successfully without any disk rebuild.
The corresponding historical snapshot suite passes 2,898 checks.

An initial core check assumed that virtual page `0x404` collides with page 5.
That is true for 1,024 entries but false for 4,096. The fixture now finds a
real colliding page and builds its page-table mapping. Its RAM, helper count,
tag, epoch and wrap assertions remain intact. Both capacities pass all
354,948 core checks with that fixture; the private prototype also passes
3,500 batch/timer checks. This test improvement is retained independently of
the rejected runtime change.

[Exact private patches, commands, tests, paired results and limitations](measurements/tlb-capacity-trial-2026-10-26.json)
are preserved. No fresh installer acceptance claim is made for this rejected
prototype. Existing external snapshots and disposable compressed children were
used; successful children were removed by the harness.
