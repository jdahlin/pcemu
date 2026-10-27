# Reusable native translations

The executable arena remains 16 MiB by default. Unlinked native bodies now have
a second, non-executable cache of emitted ARM64 words and their owning helper
descriptors. Recycling the arena expires entry addresses and clears the fast
lookup table; validated bodies can be copied back without decoding or emitting
again. Intra-body relative branches and absolute heap-backed descriptor pointers
remain valid. Collection and restoration run outside generated frames.

Raw retained code is limited to twice the executable arena size (32 MiB with
the default). When full, the cache evicts low-use bodies until it reaches 75%
of that limit, protecting the body currently being installed. Surviving heat
counts decay so previous phases cannot permanently occupy the cache. This limit
covers raw code images; descriptors, guest-byte snapshots and optional debug
records have additional memory costs. Images are RAM-only, not persistent files.

A physical page generation still detects writes to translated bytes. Before
recompiling an unlinked, single-page body after such a mismatch, the dispatcher
compares the original decoded bytes against current RAM. Compact runs preserve
discontiguous fetches from followed jumps. Identical code keeps its translation
and updates the generation used by guarded memory helpers. Changed bytes rebuild
normally. This pure comparison does not page-walk, raise faults or invoke device
callbacks. Mapping, privilege, host-buffer, watch and other context checks still
apply; restoring an emitted image never bypasses those checks.

Direct-linked blocks retain whole-arena reset and generation-rebuild behavior:
their patched cross-body branches and generation guards cannot be restored as
ordinary unlinked images. Cross-page bodies also retain conservative generation
invalidation. `clear()` releases both cache tiers and resets their counters.

Native debug maps publish a fresh address lifetime on each restore, retaining
the original guest identity and instruction regions even after an explicit map
flush. Enabling mapping after compilation uses a whole-body region when no
instruction-region metadata was captured originally.

`jit-cache-tests` forces repeated reuse and finite-cache eviction with a 64 KiB
arena, native FP/memory bodies and decoded x87 boundaries. It checks all block
modes, paging and linking combinations against the interpreter; unchanged-page
neighbors, same-value stores, actual instruction modifications, watched writes,
clear semantics and debug-map publication are covered. The cached-batch and x87
suites exercise ordinary execution alongside this focused regression test.

The benchmark window reports restored bodies/bytes, byte-validated bodies,
image evictions and end-of-window image occupancy separately from compilation
and executable-arena recycling. Counts describe different work: a restore is
not a new compilation, and a page-generation mismatch is not proof that a body
changed. Continue using the fixed 50-frame timedemo and separate uninstrumented
paired timings from host/counter profiles.
