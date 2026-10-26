# Address-bias TLB trial (26 October 2026)

The private prototype stores physical-page-base minus virtual-page-base in the
existing 32-bit frame slot. Translation becomes wrapping 32-bit addition.
ARM64 removes one instruction per paged inline memory access; CPU hits,
instruction fetch and repeated-string fast paths use the same representation.
The codec converts bias to the legacy physical frame on save and back on load,
so snapshot bytes and the 20-byte in-memory entry size are preserved.

354,964 core checks, 2,900 snapshot/COW checks, 255,535 native read checks,
947,820 RMW checks, 2,957,189 byte checks, 17,861 indirect jump checks,
91,745,234 flag-liveness checks and 43,600 stack checks pass. Synthetic
TLB fixtures are adapted to the new representation without changing expected
physical addresses, permissions or serialized legacy records. No fresh full
OS/installer acceptance was run for this private prototype.

Six alternating headless pairs per task, including reversed-order confirmation,
reuse existing compressed Office checkpoints. Median baseline/candidate process
CPU ratios are 1.02333 for loaded Excel and 0.99594 for Word launch. Pair ranges
are 0.911–1.187 and 0.843–1.281. This does not establish a reliable overall win;
the default remains unchanged.

Three repeated RAM-only pairs without our overlapping builds show paged JIT
ratios of 1.042 / 1.036 / 1.224 / 1.226 at 4 KiB / 32 KiB / 512 KiB / 8 MiB.
However, the unchanged flat 8 MiB control also improves 1.695, so these cannot
be attributed entirely to one removed translation instruction. Host scheduling,
cache/frequency effects and code layout remain confounders. An earlier RAM
series overlapped our compilation; it is retained separately and excluded from
performance conclusions. Both series pass exact guest results.

[Evidence](measurements/tlb-bias-2026-10-26.json) includes executable hashes,
commands, source/test patch, raw application pairs, correctness outputs and both
RAM series. No VM/media fixture is recreated. This local arithmetic change
helps establish a compatible additive representation, but is not a substitute
for access-specific TLB tags and a cached host-address addend. Those require
explicit handling of RAM relocation, A20/context changes, MMIO, permissions,
dirty-bit transitions and code invalidation before replacing current guards.
