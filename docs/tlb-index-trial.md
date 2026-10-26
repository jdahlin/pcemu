# TLB index simplification trial (26 October 2026)

Keep the shipping hash `(page ^ (page >> 10)) & 1023`. A private same-source
O3 prototype uses `page & 1023` in all CPU and JIT lookups, including instruction
fetch and repeated-string paths. Emitted ARM64 removes one XOR instruction;
entry layout, capacity, permissions and dirty handling remain unchanged.

Three alternating RAM-only pairs per backend execute exact checked guest work.
The ordinary 2 MiB data-base fixture shows small-case paged JIT ratios around
1.01–1.04. Those are not reliable evidence of an instruction-cost improvement:
the unchanged flat 8 MiB control varies from 0.287 to 0.938 seconds in baseline
and 0.278 to 0.560 seconds in candidate. Shared-host effects and possible code
layout effects prevent attributing these timings to the hash instruction.

A second fixture moves the same ring from 2 MiB to 4 MiB. Code remains at
zero. For the 4 KiB ring, page 0 and page 1024 select the same slot with the
simplified index but different slots with the shipping fold. At two million
iterations, all three pairs produce the exact result and 8,000,001 guest
instructions. Baseline records zero slow-memory calls; candidate records
1,999,993. Repeated instruction-fetch validation and the data load evict each
other's translations. The median timing ratio is 0.437, but the deterministic
helper-count difference is stronger evidence than these noisy wall times.

The simplification is rejected without an Office or installer acceptance run.
Saving one instruction on a hit is insufficient when a plausible compact
working set starts missing almost every load. This does not prove the current
hash optimal; any replacement needs explicit code/data alias cases alongside
capacity, dense-hit and application workloads. A split instruction/data cache
would be a separate design with its own invalidation and footprint costs.

[Evidence](measurements/tlb-index-2026-10-26.json) retains ordinary and alias
pairs, executable hashes, the private source patch, build command and the
one-line benchmark-base patch. Both fixtures use RAM only, no disk or media.
The prototype has benchmark correctness checks but no full core/OS acceptance;
production code remains unchanged.
