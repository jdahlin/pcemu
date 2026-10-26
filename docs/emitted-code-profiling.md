# Mapping generated ARM64 samples to emitted regions

The [current timestamped JIT debugging workflow](jit-debugging.md) is integrated
with Instruments and handles ordinary arena recycling. The experiments below
used older private source and maps without code lifetimes.

Three restored headless Excel dependent-recalculation profiles now map anonymous
ARM64 frames to regions emitted by the JIT. They use the native PUSH source set,
the existing checkpoint/task and verified result pixels. This resolves part of
the anonymous-code gap identified in [Office profiling](office-current-profiling.md).

## Diagnostic boundaries

The frozen diagnostic source differs only in `main.cpp` (128 MiB virtual arena)
and the emitter (region-map logging). Production runtime code is unchanged.
The larger arena prevents address reuse: all three maps and normal-quit trailers
independently confirm zero cache recycles. The emitted code occupies roughly
24 MB per process, exceeding the production 16 MiB arena. That makes larger
cache retention worth an independent test, but these diagnostic timings are not
a normal-arena speed comparison. Mapping/logging, sampling, host contention and
capture/quit margins affect collection.

A region map records half-open host address ranges, emitted operation boundaries
and a process PID. The analyzer rejects a different sampled PID and overlapping
or reused code addresses. It excludes the named profiler attachment barrier,
uses exclusive leaf counts, keeps workers outside main-thread totals and leaves
unmapped samples explicit. Duplicate offsets for operations emitting no code
have zero range. Named helper descendants inherit the nearest mapped anonymous
caller; they are reported separately from generated self samples.

## Results

The three runs contain 6,352, 6,285 and 6,386 main-thread task samples (19,023
total). Maps resolve 4,596 samples, 24.2% of main-thread task samples; the remaining
samples include known interpreter/dispatcher/device/capture work as well as any
unresolved frames. They are not all unknown code.

Largest **generated self** buckets, as percentages of all main-thread task
samples:

| Emitted region | Samples | Share |
| --- | ---: | ---: |
| Block finishing | 1,732 | 9.10% |
| Memory read | 843 | 4.43% |
| Stack operation | 517 | 2.72% |
| Prologue | 496 | 2.61% |
| Memory write | 330 | 1.73% |
| CALL | 174 | 0.91% |
| RET | 140 | 0.74% |
| ALU | 129 | 0.68% |

These are sampled wall-stack shares, not exact CPU durations, guest instruction
costs or individual ARM instruction latency. In particular, block finishing
includes GPR/IP commits, lazy flags, branch decision, instruction count and host
epilogue. Its 9.1% cannot be attributed entirely to flag calculation. Memory
regions include guards, address calculations and replay exits as well as the
actual access. CALL helpers add a separately reported 93 samples (0.49%).

The next investigation should separate flag/branch work from register commit
and return inside the finishing bucket. This evidence does not support choosing
register arithmetic as the sole primary target. Larger cache retention also
needs uninstrumented paired application tests before a default change.

## Reproduction and evidence

[Exact diagnostic patches, build/binary identity, all three task results,
per-run/aggregate attribution and compressed raw maps/stacks](measurements/emitted-code-profile-2026-10-26.json).
All compressed artifacts have verified byte-for-byte zstd round trips. Raw maps
are removed after preservation to avoid retaining duplicate diagnostic data.
The production JIT arena and policy remain unchanged, and no speedup is claimed.

After decompressing a matching map and sample from that record:

```sh
python3 tools/analyze_jit_samples.py sample.txt map.jsonl --output attribution.json
python3 -m unittest discover -s tests -p 'test_analyze*py'
```

Twelve analyzer tests pass, including six new attribution tests for boundaries,
address reuse, skipped operations, invalid ranges, PID identity, exclusive
accounting and helper attribution. No new guest disk or Office fixture is made;
the task harness removes successful private compressed children.


## Finer block-finishing attribution

A subsequent diagnostic splits the final block exit into ordered emitted regions.
The existing analyzer consumes the finer markers unchanged; all map addresses
are unique, PID identity matches, and the task's expected pixels pass. This is
one run with 14,742 main-thread task samples on the same shared host, not another
normal-arena timing comparison.

| Final block-exit region | Self samples | Share of main-thread task samples |
| --- | ---: | ---: |
| Flags | 711 | 4.82% |
| Cached GPR commit | 187 | 1.27% |
| Host register restore/return | 129 | 0.88% |
| IP commit | 69 | 0.47% |
| Conditional branch | 50 | 0.34% |
| Guest instruction return count | 40 | 0.27% |

These finer finishing buckets sum to 8.05% in this run, versus 9.10% in the
previous three-run coarse collection. Sampling, shared-host variation, diagnostic
layout and boundary effects remain. Flag work inside memory/replay or other
operation regions stays in those regions; 4.82% is not the total cost of every
flag operation in the emulator. Still, it is the largest final-exit component.
Eliminating it alone cannot explain a route to twofold whole-task improvement.

[Exact finer diagnostic patches, build/task identity, attribution and compressed
map/stack evidence](measurements/jit-finish-profile-2026-10-26.json).
Production code and its 16 MiB arena are unchanged by this profiling. Raw maps
are removed after verified compression.
