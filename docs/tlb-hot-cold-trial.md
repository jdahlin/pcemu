# Split hot/cold TLB trial (26 October 2026)

The default remains the 1,024-entry, 20-byte TLB. This private prototype keeps
1,024 entries but separates the original PTE address into a cold 4 KiB array.
Each aligned hot entry becomes 16 bytes, so indexing changes from a multiply
by 20 to a shift by four. Total storage and `sizeof(CPU)` remain unchanged:
20 KiB and 21,072 bytes respectively. This differs from the earlier packed
entry experiment: frame addresses retain their original representation.

The slow path remembers the PTE address in the corresponding cold slot.
The snapshot codec preserves the existing serialized field order. ARM64
loads use the new permission/dirty offsets. No production source is changed.

Three alternating CPU-only O3 dependent-load pairs per backend pass exact
result checks. At 8 MiB the median baseline/candidate ratios are 1.125 for
paged JIT and 0.972 for flat JIT. These are shared-host timings, not isolated
memory latency. The small paged cases are approximately unchanged; 512 KiB
paged JIT is 1.030. Table size alone does not explain performance.

Six restored headless pairs per Office task, including a reversed-order
confirmation series, pass expected-screen checks. Existing compressed
checkpoints are reused; no Office installation or large disk is created.
Excel's median process CPU ratio is 1.05124, equivalent to about 4.9% less
CPU time. All six pairs favor the prototype (1.029–1.098). Word's median is
0.97666, about 2.4% more CPU time, with three pairs favoring each layout and
a wide 0.900–1.152 range. Response ratios are 1.06659 and 0.94700 respectively.
Word measures launch, whereas Excel measures dependent recalculation;
neither should be generalized to all application work. CPU includes control
and capture costs. No confidence interval or statistically established Word
regression is claimed.

Correctness passes: 354,964 core checks, 2,900 snapshot/COW checks, 255,535
native read checks, 947,820 RMW checks, 2,957,189 byte checks, 17,861 indirect
jump checks, 91,745,234 flag-liveness checks and 43,600 stack checks. These
include the retained original-PTE dirty-address regressions from b62809b.
This prototype has not received a new full installer/OS acceptance run.

The useful Excel result warrants retaining the experiment, but mixed Word
results and flat controls do not justify a default layout change. A future
host-addend/comparator design should be compared against this prototype as
well as the shipping implementation, with loaded application work included.

[Retained evidence](measurements/tlb-hot-cold-2026-10-26.json) includes raw
paired results, binary hashes, build commands, correctness output, layout
sizes and the source/test patch. Private executables remain ignored under
`build/tlb-hot16`; no installation media or VM disks belong to this change.
