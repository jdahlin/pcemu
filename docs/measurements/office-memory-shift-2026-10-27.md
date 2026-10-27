# Word and Excel: native memory shifts and rotates

Memory SHL/SAL/SHR/SAR and ROL/ROR/RCL/RCR now lower to the guarded native RAM
path, for byte/word/dword operands and implicit-one, immediate or CL counts.
They share the existing register result/count/flag generator. The refactor
retains the register fast path when variable dword shift flags are dead.

The lowering validates segment, translation, permissions, dirty state, RAM
bounds, A20 and code-bitmap conditions before reading. It materializes prior
flags, captures the guarded RAM address in the frame's free flag-spill area,
and keeps the input in a caller-save temporary while computing result/flags.
Pure RAM writes cannot then fault or invoke guest callbacks. Register and
memory forms use the same deterministic undefined-flag convention as the
interpreter, including preserved AF and multi-bit OF.

Masked/normalized zero counts retain a real memory operation in the IR. They
still undergo all write guards; unsafe cases replay the instruction so zero
counts can fault or invoke watched writes exactly as before. A validated
ordinary RAM zero-count hit may skip writing the unchanged value because the
write permission and dirty state are already satisfied and devices, watches
and code writes have been excluded. Cold/unsafe accesses replay before any
translation or callback rather than using a partial RMW helper. Self-PTE
accessed/dirty-bit effects therefore remain interpreter-exact on that path.

## Correctness

Core and bounded-batch suites pass 354,964 and 112,885 checks. Cached validation
passes **20,421,602 checks**, with links on/off and 103,944 observed linked
transitions. Existing register-rotate and division exhaustive tests remain in
that total. New small RAM-only tests exhaust byte values and all masked counts,
all eight group operations, immediate/CL forms and four carry/overflow/other
flag images. They require actual native execution without a memory helper.

Wider cases cover signed boundaries, count/operand-width boundaries and large
counts, code/operand/address widths, FS/SIB addressing, cold versus warm dirty
pages, and explicit native-hit assertions for every width. Tests also compare
zero-count watched/MMIO reads/writes, normalized full-ring counts, read-only and
cross-page write faults without partial state, self-PTE aliases, code-changing
writes, CL as both address and count, deferred IMUL flags before a zero rotate,
and memory ROR followed by ADC through loops in basic/follow/trace modes.
All compared RAM, registers, flags, instruction/IP state and callback traces
match the interpreter.

## Application protocol

Original-A/B use the same frozen 57ce863 executable. Prior is the frozen
b79d1eb division build, and the candidate comes from a clean archive plus the
selected x86 changes. Word uses the unchanged 4,807,680-byte document checkpoint,
original task hash and first-page title pixel gate. Excel uses its existing
dependent-recalculation checkpoint/task. Headless runs keep direct links off,
default jump following, and small private compressed deltas removed on success.
No experiment builds or profilers overlap timing; instrumented diagnostic
measurements are separate and excluded from scores. Existing host background
activity remains; the separately running VM was not paused without approval.
Word's title gate does not measure complete background pagination. Process CPU
includes guest, compilation, device and capture/control work, excluding restore,
shutdown, blocked and descheduled time.

## Coverage diagnostics

| Interpreted memory operands | Prior Word | Candidate Word | Prior Excel | Candidate Excel |
| --- | ---: | ---: | ---: | ---: |
| C0 byte immediate group | 0 | 0 | 560 | 7 |
| C1 immediate group | 118,272 | 119 | 13,178 | 40 |
| D0 byte implicit-one group | 17 | 17 | 2 | 2 |
| D1 implicit-one group | 6,407 | 577 | 546 | 245 |
| D2 byte CL group | 392 | 21 | 16 | 7 |
| D3 CL group | 13,014 | 56 | 3 | 3 |

Total interpreted attempts are 5,650,310 → 5,482,019 in Word and 8,145,011 →
8,169,108 in Excel. Excel's small family-specific reduction does not imply a
reduction in every diagnostic count: scheduling and other guest work can vary.
Remaining supported forms may fail admission or hit replay guards. Counts
include faults and REP host-batch resumptions and are not host CPU attribution.

## Matched original-baseline results

All **48 unprofiled restores** reach the expected pixels and exit cleanly:
eight rotating Word rounds and four Excel rounds, each with original-A,
original-B, prior and candidate. No scores are discarded.

| Workload | Original-A CPU | Original-B CPU | Prior CPU | Candidate CPU | Prior → candidate native share |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word | 1895.7 ms | 1896.9 ms | 1478.2 ms | 1462.4 ms | 94.31% → 94.44% |
| Excel | 7281.4 ms | 7281.4 ms | 6626.0 ms | 6617.9 ms | 99.428% → 99.429% |

The complete candidate's median process CPU is **22.9% below the pooled original
Word baseline** and **9.1% below the pooled original Excel baseline**, beating
both original controls in every round. Only the original controls rerun in this
comparison are used; older absolute scores are not compared across sessions.

Word CPU is 1.07% below the prior division build, with all eight paired rounds
winning (0.63–3.08% reduction). Median absolute A/A variation is 0.50%, maximum
0.89%, so this is a modest but consistent improvement in this comparison.
Excel CPU differs by only -0.12%, with mixed paired differences (-0.22–0.44%);
A/A median is 0.20%, maximum 1.03%. Its incremental timing is neutral at this
resolution. Readiness remains the original gate, not full document pagination.

The host was not idle. A snapshot near the end showed the separate PCEmu
Library process at 29.3% CPU and coreaudiod at 10.9%. Those sessions were not
altered. This single observation is not integrated load accounting and does not
prove a particular cause of timing variation. Unlike the previous experiment,
the A/A controls here remain relatively consistent; their actual variation is
reported above rather than assuming either an idle or unusable host.

The next coverage selection should examine remaining opcode costs as well as
counts, now that the large memory shift gap is closed. The broader goal remains
active.

[Full timing records, selected source hashes, counters and test logs](office-memory-shift-2026-10-27.json)
retain every run and the background observation. Build products, raw traces and
guest images remain local and ignored by Git.

A separate Word Instruments recording with validated timestamped JIT maps has
1,584 main-thread task samples: generated code/helpers 23.7%, dispatch/cache
validation 22.6%, interpreter/MMU 20.6%, compilation 18.9%, capture/video/control
7.7%, polling/interrupts 4.0%, other work 2.0%, disk/compression 0.4%. Sample shares
are diagnostic, not isolated function durations or proof of one opcode's cost.
The profiled timing is excluded from performance scores. Analysis is saved in
the JSON; raw traces/maps remain under `build/office-memory-shift/`.

[Compressed candidate guest counters and sampled IPs](office-memory-shift-2026-10-27.profiles.zst)
retain both workloads and pass a decompression round-trip check. Frozen
executable identities and task/control settings were verified; selected source
files match the build source byte-for-byte. Prior profiles remain with the
division report.
