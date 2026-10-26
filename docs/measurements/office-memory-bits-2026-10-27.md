# Word and Excel: native memory bit strings

BT/BTS/BTR/BTC memory forms now use generated ARM64 for guarded RAM accesses,
including immediate and register indexes with 16/32-bit operand and address
sizes. Word's preceding profile showed 307,269 interpreted memory BT operands;
Excel's showed 80,301. Immediate bit-test groups also appeared in both workloads.

Register indexes are signed at operand width. Generated code wraps the ordinary
effective offset at address width, then adds the signed word displacement with
arithmetic shift (floor for negative indexes). It does not wrap that adjustment
again at 16 bits. Immediate forms select a bit within the addressed word,
matching the interpreter's existing behavior. Segment overrides remain active.

Only CF changes; all other flags retain the interpreter's exact values. Pending
flags are materialized before the access so replay and fault restart remain
precise. Modifying forms capture the original input before write translation,
including self-PTE dirty-bit aliases, and publish CF after success. Watched/device
accesses, crossing-page accesses and faulting permissions use the existing
helper/replay path. Code writes terminate the native block through the completed
instruction. Explicit LOCK prefixes retain interpreter execution.

## Validation and scope

Core and bounded native batch tests pass 354,964 and 112,885 checks. Cached batch
validation passes 236,438 checks with links on/off and observes 92,305 linked
transitions. New interpreter comparisons cover all four operations, immediate
indexes, negative/positive register indexes through the signed 16-bit endpoints,
independent operand/address/code widths, FS/SIB addressing, wrapping before
index adjustment, source/address aliases, exact CF and preserved flags, pending
flags followed by carry consumers, loops in basic/follow/trace modes, cold TLB
helpers, self-PTE aliases, self-modifying code, exact MMIO/watch callback ordering,
and read-only/not-present/cross-page faults without partial data changes.
Separate assertions require actual native execution of each operation.

Performance comparisons use the unchanged original Word 4,807,680-byte document
checkpoint and pixel gate, and the existing Excel dependent-recalculation
checkpoint/task. Runs are headless, with direct linking disabled, default jump
following, and private disposable disk deltas. The original executable remains
57ce863; the prior build is the frozen 2a8a4ef XCHG executable. Timed runs have no
concurrent builds or profilers. Separate instrumented runs provide diagnostic
counts, whose timings are excluded. Word's gate measures first-page rendering,
not completion of background pagination. Both workloads retain all compilation,
control/capture and device work in their process CPU measurements.

## Results against the original baseline

All **48 unprofiled independent restores** reach the expected pixels and exit
cleanly. Word has eight rotating rounds and Excel four, each comparing original-A,
original-B (identical executable), prior XCHG, and candidate. No runs are discarded.

| Workload | Original-A CPU | Original-B CPU | Prior CPU | Candidate CPU | Prior → candidate native share |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word | 1989.9 ms | 2011.5 ms | 1655.2 ms | 1623.5 ms | 92.85% → 93.25% |
| Excel | 7591.8 ms | 7631.1 ms | 7087.6 ms | 6976.4 ms | 99.33% → 99.34% |

Median candidate process CPU is **18.7% below the pooled original Word baseline**
and **8.3% below the pooled original Excel baseline** in this fresh experiment.
Every round beats both original controls. The reduction versus prior medians is
1.9% in Word and 1.6% in Excel. Word wins seven of eight paired prior comparisons;
the first round is 4.1% slower and is retained. Word's median absolute A/A
difference is 1.03%, maximum 3.02%. Excel wins all four paired comparisons, but
the gains range from 0.2–4.1% and its A/A median is 1.98%, maximum 3.82%. The
incremental gains are modest and Excel's precise magnitude remains uncertain.
Absolute scores from older sessions are not used for comparison.

Diagnostic interpreter attempt counts fall as intended:

| Opcode | Prior Word | Candidate Word | Prior Excel | Candidate Excel |
| --- | ---: | ---: | ---: | ---: |
| 0F A3 BT | 307,277 | 1,009 | 80,316 | 397 |
| 0F AB BTS | 4,507 | 236 | 11,024 | 138 |
| 0F B3 BTR | 5,051 | 508 | 11,227 | 337 |
| 0F BA immediate group | 59,508 | 2,141 | 81,816 | 1,148 |
| All interpreted attempts | 7,017,898 | 6,663,121 | 9,519,563 | 9,336,580 |

These counts include faults and REP host-batch resumptions; profile guest
instruction totals can vary with timer scheduling. Counts measure coverage,
not attribution of host CPU time. BTC did not appear in these two task profiles,
but has the same directed native/interpreter validation as the other operations.

The next shared missing form remains LEAVE: candidate profiles show 214,492 Word
attempts and 160,511 Excel attempts. Memory IMUL also remains important to Word.
The broad native-coverage objective is still active.

[Full timing records, source hashes, profile summaries and test logs](office-memory-bits-2026-10-27.json)
are retained with this report. The original baseline and task hashes remain in
every timing record. Local builds and disk images remain outside Git.

A separate Instruments recording has 1,747 main-thread task samples:
dispatch/cache validation 23.4%, generated code/helpers 22.2%, interpreter/MMU
20.7%, compilation 16.8%, capture/video/control 7.2%, polling/interrupts 6.3%,
other host work 3.3%, and disk/compression 0.2%. These are sample shares, not
isolated function durations. The leading costs remain spread across dispatch,
native execution and interpretation; an opcode count is not proof of a single
host-time bottleneck.

The first diagnostic trace was rejected by the JIT map validator after a 96 µs
backward system-clock step. Its guest completed and exited normally. That trace
is retained locally but excluded from attribution; a fresh recording passed
clock/lifetime validation and supplies the sample shares above. This diagnostic
retry does not discard any of the 48 unprofiled performance scores.

Raw traces/maps remain local in `build/office-bits/`.
[Compressed candidate guest counters/IP samples](office-memory-bits-2026-10-27.profiles.zst)
are saved for both workloads and verified by decompression round-trip.
Prior XCHG profiles are retained with the preceding report.
