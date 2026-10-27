# Reusable translations: 50-frame checkpoint

Implementation `895b776`, compared with the native-x87 baseline `8305377`.
Five alternating pairs of frozen optimized binaries run the same silent 320×200
50-frame prefix through the shared `runBatch(64)` path. The executable arena is
still 16 MiB. A bounded non-executable image cache adds up to 32 MiB of raw code,
plus descriptors and compact decoded-byte snapshots.

| Metric | Native-x87 baseline | Reusable translations |
| --- | ---: | ---: |
| Median FPS | 101.48 | 151.64 |
| FPS range | 101.09–102.77 | 150.12–151.91 |
| CV | 0.74% | 0.49% |
| Median wall ms/frame | 9.854 | 6.595 |
| Median thread CPU ms/frame | 9.837 | 6.586 |
| Compiled blocks / 50 frames | 32,156 | 898 |
| Arena resets / 50 frames | 5 | 0 |

**Median paired improvement: 1.494×.** Pair ratios:
1.478, 1.494, 1.496, 1.464, 1.502. The overall 5–10× objective
is not achieved by this checkpoint; these ratios should not be multiplied across
historical cohorts and presented as a fresh paired overall measurement.

Compilation falls 97.2%. The candidate restores a median 708 bodies (696,688
raw bytes), validates 20,399 unchanged bodies after physical-page generation
changes, and evicts no images inside the timed window. End-of-window image
occupancy is 15,969,212 bytes. The earlier arena-compaction experiment only
reached about 106 FPS and was discarded; page-wide false invalidation was the
larger compilation source. Restoring code images alone also left substantial
recompilation until exact-byte revalidation was added.

A separate strict coverage run passes with 100% compiled application coverage,
zero interpreter steps and zero predecode interpreter calls in the exact timed
window. Coverage excludes DOS/CWSDPMI. 332,694 focused cache checks, 737,753 x87
checks, 20,456,574 cached-batch checks and boundary/context/benchmark suites pass.
Eighteen restored and late-enabled debug maps pass the timestamped-map parser.
Fourteen Python benchmark/profiling tests pass.

For isolation, candidate sources are the exact frozen native-x87 inputs plus
only this commit's cache build inputs. Concurrent VGA changes are excluded.
[Every run, source/binary hash, counter and coverage result](quake-code-cache-2026-10-27.json)
records the overlay explicitly. Frozen binaries, manifests, logs and the demo
are in `build/quake-cache-final/`. Successful temporary compressed guest overlays
are removed; no full demo or large VM fixture is used. Host-clock timing causes
some instruction-count variation; matching frames and demo bytes define useful
work. The spread is not a statistical confidence interval.
