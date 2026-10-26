# Same-page ARM64 direct-link experiment

`--jit-direct-links` enables an experimental native edge within the existing
bounded batch. It remains off by default. `--jit-link-warmup N` selects native
entries before promotion (CLI default 256; zero requests immediate emission).
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

Incoming edges are tracked with weak metadata references. Before replacing a
target, its incoming branch slots are restored to their source's fallback exit,
while no native frame is live. This protects helper metadata embedded in old
code. Cache recycling drops all links before arena reuse. Patching uses macOS
JIT write protection and instruction-cache synchronization.

The root frame reserves separate link-control slots beyond the existing
operand, carry and indirect-call temporaries. A carry-spill regression test
guards against the initial frame-slot collision found by the Word smoke task.
The existing soft 64 / maximum 127 instruction budget and logical dispatch
calendar remain the interrupt/control boundaries.

## First implementation result

The [preliminary evidence](measurements/direct-links-inline-2026-10-26.json)
does **not** support a Word speedup. Immediate emission uses roughly 1.6 million
direct transitions in a verified launch but adds code and CPU cost. Promoting
only hotter blocks reduces adoption and duplicates translated bodies. The
retained prototype is therefore experimental while promotion is being changed
to avoid full-body retranslation. No application speedup is claimed.

Core, bounded-batch and cached-batch suites pass. The latter includes 142,958
checks, with independent interpreter comparison, real links, self-modifying
code, code-PTE remapping, precise faults, MMIO exactly-once replay and buffer/
context changes. Timer tests exercise interrupt deadlines with links enabled.
All profiling and application probes are headless and use the existing local
Word checkpoint with disposable compressed children.

JIT debug sidecars now use version 2 and include timestamped four-byte `patch`
events. The analyzer accepts both versions 1 and 2; patches preserve block/region
identity while retaining the actual emitted branch bytes for disassembly.
