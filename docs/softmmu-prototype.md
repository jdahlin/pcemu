# QEMU-style software-MMU fast-path prototype

The shipping CPU already walks guest page tables, caches translations, enforces
permissions and delivers faults. This private prototype changes the common
native memory path to access-specific comparison tags plus a cached host-address
addend. It is original implementation work, using the design described by
[QEMU's translator documentation](https://www.qemu.org/docs/master/devel/tcg.html)
and [fast TLB layout](https://github.com/qemu/qemu/blob/v10.2.2/include/exec/tlb-common.h).
It does not incorporate QEMU CPU or device implementation code.

## Implemented prototype

A derived 1,024-entry cache accompanies the reference TLB. Each 32-byte entry
contains read/write comparison keys and a host-address addend. Keys combine the
reference epoch and virtual page; a restriction bit makes user accesses miss
on supervisor-only mappings. Write keys stay invalid until the guest PTE is
dirty. Unknown permission bits conservatively force user writes through the
helper, matching the existing emitter's behavior. Whole ordinary RAM pages are
eligible; VGA/MMIO and partial final RAM pages have invalid fast keys.

Generated paged accesses retain segment-unusable and cross-page guards, then
compare the appropriate key and add the cached host offset. RAM bounds, A20
mapping, VGA exclusion and most permission checks move to filling. Stores still
check the existing translated-byte bitmap using the actual physical offset;
code writes, MMIO, faults and dirty-bit updates preserve helper/replay behavior.
Nonpaged accesses and instruction-fetch validation retain their reference paths.
This is a first stage, not a complete replacement of every MMU consumer.

RAM pointer/size, A20 and execution-epoch changes retire derived mappings before
native entry. TLB epochs handle address-space flushes; wraparound clears derived
keys. Reference-slot eviction invalidates the corresponding derived entry.
Host addresses are never serialized; the snapshot format remains unchanged.

The first version filled the derived cache on every reference slow walk. The
second fills it on successful native helper accesses and initial JIT attachment,
while interpreter-side reference refills only invalidate corresponding derived
entries. Both exact source variants and their observations are retained.

## Correctness evidence

The frozen second-version executable passes headless DOS boot and all three
reset paths, Windows 3.11 Notepad save with exact 25-byte readback, and both rtts
and installed OSR2 Calculator/clipboard/Notepad/restart checks with exact stopped
38-byte readback. Successful private disks are removed; existing compressed
parents and checkpoints are reused. Fresh Setup has not been repeated with
this prototype; previous shipping Setup evidence cannot establish that gate.

354,964 core checks, 2,900 snapshot/COW checks, 2,061 derived-cache checks,
255,535 native read checks, 947,820 RMW checks, 2,957,189 byte checks, 17,861
indirect-jump checks, 91,745,234 flag checks, 43,600 stack checks and 71,249
cached-batch checks pass. The latter exercise 4,440 multi-dispatch batches.
Tests directly editing synthetic reference-TLB metadata explicitly refresh the
derived cache; expected faults, addresses and helper-count assertions remain.

OSR2 first reached Explorer but the application checker repeatedly sent Escape
to an inactive delayed Welcome window. An unchanged reference passed a control;
a focus-verified checker then exercised recovery on the same frozen prototype
and passed the full application/restart/persistence gate. See
[Welcome recovery](win95-welcome-recovery.md), including the initial failure.

## Performance and next work

Three uninstrumented headless pairs per version use existing Office checkpoints.
Baseline/candidate process CPU ratios above one favor the prototype:

| Variant | Excel dependent recalculation | Word warm launch |
| --- | ---: | ---: |
| Fill on reference slow walks | 1.0602 | 0.9256 |
| Fill on native accesses / initial attachment | 1.0372 | 0.9416 |

Both series favor loaded Excel but regress Word launch at the median. These
small, noisy series are not confidence estimates or a global win. Initial RAM
comparisons favor several paged cases, but unchanged flat controls also vary
substantially, even with process CPU accounting; they do not isolate the cost
of translation. Scheduling, frequency, shared caches and layout remain factors.
No speedup over QEMU or cumulative twofold application gain is established.

The additional hot cache occupies 32 KiB; CPU grows from 21,072 to 53,888 bytes.
That footprint and duplicated metadata are costs, not proven explanations for
the Word result. Next compare a separately allocated fast-table descriptor and
profile the regressing workload before default integration. Longer term, unify
reference and fast metadata, evaluate MMU-context specialization and instruction
fetch, and preserve all invalidation and fault/replay gates. Direct block chaining
remains a separate major dispatch optimization.

[Evidence and reproducible source/test patches](measurements/softmmu-prototype-2026-10-26.json)
include both variants, source/binary hashes, commands, correctness outputs and
OS persistence records. Private sources/binaries remain in ignored
`build/softmmu-tags`. The shipping runtime is unchanged by this report.

## Word follow-up

[The subsequent Word investigation](word-softmmu-investigation.md) finds millions
of unnecessary context checks on interpreted dispatches and moves them to native
entry in a validated private variant. Larger balanced timing controls do not
reproduce a stable overall 6% Word penalty; the small-series table above remains
historical evidence, not the current confidence in a general regression.
