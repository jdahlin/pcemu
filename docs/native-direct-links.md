# Same-page ARM64 direct-link experiment

`--jit-direct-links` enables an experimental native edge within the existing
bounded batch. It remains off by default. `--jit-link-warmup N` selects native
entries before promotion (CLI default 256; zero requests immediate promotion).
Both complete process CPU time and exact guest results are acceptance gates.

The first implementation patches an ARM64 B instruction to a compatible native
entry on the same guest linear and physical code page. It has a common host
register-save layout and preserves live guest registers when the successor's
register set is a subset of the source's. Self backedges skip their own register
and pointer initialization. Source state commits before a completed edge.

Only supported native operations execute inside a linked path. Slow memory
helpers already verify source code generation after their operation; inline
stores reject marked code bytes. A per-block flag records slow helper calls.
Only those calls can evict the cached fetch translation, so their completed
edges recheck fetch tag, epoch, physical page and user permission. A missing
translation returns to ordinary lookup and fault handling. Partial exits and
replays always return to C++ with accumulated instruction and dispatch counts.

Incoming edges record the source guest key and native entry address. Each block
uniquely owns its link metadata; incoming edges never dereference freed metadata.
Native entries are unique within an arena lifetime, and recycling clears incoming
edges before addresses can be reused. Before replacing a
target, its incoming branch slots are restored to their source's fallback exit,
while no native frame is live. This protects helper metadata embedded in old
code. Cache recycling drops all links before arena reuse. Patching uses macOS
JIT write protection and instruction-cache synchronization.

The root frame reserves separate link-control slots beyond the existing
operand, carry and indirect-call temporaries. A carry-spill regression test
guards against the initial frame-slot collision found by the Word smoke task.
The existing soft 64 / maximum 127 instruction budget and logical dispatch
calendar remain the interrupt/control boundaries.

## Promotion without retranslation

Promotion retains the original guest body. Cold candidates reserve one exit
hook and, for paged memory, helper flag hooks. A hot entry receives small root,
completed-exit and partial-exit stubs; branch patches connect these to the body.
The stubs occupy a separate bounded slab (up to 1 MiB) adjacent to the 16 MiB
body arena, so promotion cannot cause earlier guest-body cache recycling.
Saturation skips further promotion until the next clear/recycle. All patches
happen outside native execution. The shared frame is 128 bytes, with link
counters at offsets 112/116 and helper flag at 120; ordinary operand and carry
spills remain at 96..108. A delayed-promotion test requires exactly one guest
compilation, so it detects accidental full-body retranslation.

The [initial inline evidence](measurements/direct-links-inline-2026-10-26.json)
and [large-document benchmark](measurements/word5m-m1-2026-10-26.md) do **not**
support an application speedup. The latter exercises about 100 million guest
instructions and 5.3 million linked transitions, but linked CPU time remains
higher in the preliminary comparison. The prototype remains opt-in. No
application performance acceptance gate has passed.

Core, bounded-batch and cached-batch suites pass. The latter includes
checks with independent interpreter comparison, real links, self-modifying
code, code-PTE remapping, precise faults, MMIO exactly-once replay and buffer/
context changes. Timer tests exercise interrupt deadlines with links enabled.
All profiling and application probes are headless and use the existing local
Word checkpoint with disposable compressed children.

JIT debug sidecars now use version 2 and include timestamped four-byte `patch`
events. The analyzer accepts both versions 1 and 2; patches preserve block/region
identity while retaining the actual emitted branch bytes for disassembly.

Body and stub publications can interleave in address order. The debug-map loader
maintains a sorted interval index and checks both neighbors on insertion.
Direct links reject misaligned arenas and allocations spanning 128 MiB or more;
constructor allocation arithmetic also rejects overflow.

## Selectivity and metadata experiments

A self-loop-only emitter restriction was tested and rejected: five rotating
Word document-launch rounds yielded median CPU 2143.1 ms baseline, 2227.7 ms
self-only warmup 256, and 2211.8 ms self-only warmup 32. It reduced promotions
to roughly 100–200 but removed most useful cross-block transitions. The source
restriction is not retained; the exact experimental patch remains local in
`build/direct-links-self/source.patch`. Individual measurements are in
[the self-link report](measurements/direct-links-self-word5m-2026-10-26.json).

Replacing shared link metadata ownership with unique ownership and native-entry
identity removes a control block and reduces the link field in every cached
block from two pointers to one. A new replacement/recycling test repeatedly
changes both ends of a linked loop containing a memory helper, compares every
batch against the interpreter, and requires real links and arena recycling.
The cached-batch suite now passes 151,150 checks. This representation alone
still does not pass the performance gate: five rotating rounds gave median CPU
2023.9 ms baseline, 2101.2 ms warmup 256 and 2089.0 ms warmup 1024. Those runs
are in [the ownership report](measurements/direct-links-unique-word5m-2026-10-26.json).
Absolute timings across separate experiments should not be compared as speed
changes; each experiment contains its own rotating baseline measurements.
