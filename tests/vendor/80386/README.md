# Pinned 80386 hardware vectors

Upstream: [SingleStepTests/80386](https://github.com/SingleStepTests/80386), Daniel
Balsom, Intel 80386EX hardware. MIT license: see LICENSE.

`UPSTREAM.json` pins the source revision and Git blob identities of the compressed
MOO files and metadata. Large data files are downloaded only by explicit extended
test targets and stored under ignored `build/x86-external/80386/`.

`quick.json` is an offline architectural-state excerpt: the first two
non-exception vectors from each default opcode/size shard, the first LOCK #UD
vector from opcode 00, and the first two #DE vectors from each byte/word divide
shard. Revoked vectors are excluded. This is a semantic sampling policy, not a
selection based on whether PCEmu passed. It intentionally leaves broad segment
boundary, instruction-fetch boundary and stack-boundary sweeps to the extended
target. The selected vectors retain their original indices and hardware hashes.
The vendored excerpt's SHA-256 is pinned too.

Cycle and effective-address convenience chunks are omitted from the excerpt.
Expected registers and memory remain hardware results. There are 61 vectors;
`make test` runs them against both interpreter and JIT, including interpreter
fallbacks where the JIT has no lowering. Native instruction counts are reported.
The corpus uses a 386EX with a 16-bit bus, so this adapter makes no claim about
386DX bus-cycle timing or the prefetch queue.
