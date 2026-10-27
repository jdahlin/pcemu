# Word and Excel: native integer division

DIV/IDIV now lower to ARM64 for byte/word/dword register operands and guarded
memory RAM hits. Quotient and remainder use the original AL/AH/AX/DX/EAX/EDX
inputs, including divisor and effective-address aliases. Partial-register
updates preserve the other bits. Flags retain the interpreter's exact values.

Generated code checks divisor zero and quotient range before changing any guest
register. Signed dword division also rejects INT64_MIN/-1 before host division.
Only validated results commit. Error branches publish the completed prefix's
registers and flags, restore the dividing instruction's start IP, return out of
the native frame, and let the interpreter execute the operation. This preserves
the interpreter's existing runtime-error messages and post-error IP/instruction
state; it does not change division errors into a different exception model.
No C++ exception unwinds through generated ARM64.

Memory division uses the existing side-effect-free RAM/TLB/segment guards.
Cold or unsafe accesses replay before translation or a device callback, rather
than calling the ordinary read helper first. This matters because replaying an
error after a read could repeat MMIO or observe code changed by page-table A
updates. An error detected after a warm RAM read can safely replay that ordinary
read: the native read made no translation, device or guest-state changes.
Cross-page and device accesses remain exact interpreter operations. Hot valid
RAM operands execute native quotient/remainder calculation without a helper.

## Correctness

Core and bounded-batch tests pass 354,964 and 112,885 checks. Cached validation
passes **16,066,216 checks** with linking on/off and 102,196 observed linked
transitions. RAM-only tests reuse small compiled programs to exhaust all 65,536
byte dividends against five representative unsigned/signed divisor patterns,
checking successes and errors. Wider boundary matrices cover zero, sign limits,
quotient overflow, and INT64_MIN/-1, independent code/operand widths, exact upper
halves, every divisor register encoding and high-byte aliases.

Additional comparisons cover FS/SIB and 16-bit addressing, effective addresses
in implicit result registers, cold versus warm paging with asserted native/replay
counts, MMIO successes and zero errors without duplicated callbacks, not-present
and cross-page faults, self-PTE A updates before overflow, prior flags that a
following ALU would otherwise make dead, loops that reach zero after native/link
iterations, normal loops in all three block modes, and deferred IMUL product
flags surviving division and cold replay. Error messages and complete compared
CPU state match the interpreter. Tests distinguish actual native successes from
passing through interpreter fallback.

## Application protocol

The same immutable Word 4,807,680-byte document checkpoint, original title pixel
gate and task hash are used, together with the existing Excel dependent-recalc
checkpoint/task. Original-A/B use identical 57ce863 binaries. Prior is the frozen
0d7d5e0 NOT/memory-IMUL build; candidate is built from a clean archive plus the
selected x86 changes. All timing runs are headless, direct links off, default
jump following, and use small private compressed deltas removed on success.
No builds or profilers overlap timing. Profiling diagnostics are separate and
their timings are excluded. Word readiness means first-page rendering, not
complete pagination. Process CPU includes guest, compilation, device and
capture/control work, excluding restore, shutdown, blocked/descheduled time.

## Coverage diagnostics

| Interpreted attempts | Prior Word | Candidate Word | Prior Excel | Candidate Excel |
| --- | ---: | ---: | ---: | ---: |
| F6 /6 byte DIV | 653 | 107 | 167 | 48 |
| F7 /6 word/dword DIV | 181,372 | 54,922 | 29,981 | 6,988 |
| F7 /7 word/dword IDIV | 41,376 | 919 | 33,830 | 361 |
| All opcodes | 5,797,750 | 5,650,310 | 8,200,273 | 8,145,011 |

Remaining forms can hit cold/unsafe memory guards, fail native admission, or
require error replay. These counters include faults and REP host-batch
resumptions, and scheduling can change guest instruction totals. They establish
coverage effects, not the CPU cost of an individual opcode.

## Matched original-baseline comparison

All **48 unprofiled restores** pass their task gate and exit cleanly: eight
rotating Word rounds and four Excel rounds, comparing original-A, original-B,
prior and candidate. No scores are discarded.

| Workload | Original-A CPU | Original-B CPU | Prior CPU | Candidate CPU | Prior → candidate native share |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word | 2163.6 ms | 2011.8 ms | 1651.1 ms | 1691.9 ms | 94.12% → 94.30% |
| Excel | 7970.2 ms | 7703.1 ms | 7010.0 ms | 7226.6 ms | 99.424% → 99.428% |

The complete candidate's CPU median is **20.1% below the pooled original Word
baseline** and **9.0% below the pooled original Excel baseline**. Every candidate
round beats both original controls. Only this experiment's rerun controls are
used; older sessions' absolute scores are not compared.

The incremental performance result is **inconclusive**. The candidate CPU
medians are 2.5% higher than prior for Word and 3.1% higher for Excel. In contrast,
the median ratios within matched rounds are only about 0.03% higher and 0.10%
higher. Individual paired differences span both signs, including large swings.
Word's median absolute A/A difference is 2.26%, maximum 16.00%; Excel's is 0.60%,
maximum 6.09%. The experiment does not establish an incremental speedup and is
not strong enough to attribute the higher pooled medians to division lowering.
An isolated-host follow-up is needed for that conclusion.

A process snapshot during timing showed a separate running PCEmu Library app at
18.9% CPU and coreaudiod at 9.5%, alongside the benchmark process at 98.6%. That
background session was left running. This is one observation, not integrated
load accounting or proof of a specific cause of variation. It establishes that
the host was not idle; the control variation and small incremental effects must
be interpreted accordingly. Permission to pause/resume that separate VM for a
follow-up was requested, without interrupting it during this experiment.

Memory shifts remain a prominent missing native family in Word. The coverage
objective remains active; this change establishes guarded division coverage,
not completion of all relevant application instructions or a per-change speedup.

[Full records, source hashes, counters and test logs](office-divide-2026-10-27.json)
retain every timing run and the background observation. Generated traces, builds
and guest disks remain local and ignored by Git.

A separate Word Instruments recording with validated timestamped JIT maps has
1,629 main-thread task samples: dispatch/cache validation 23.9%, generated
code/helpers 22.7%, interpreter/MMU 21.1%, compilation 16.5%, capture/video/control
7.8%, polling/interrupts 5.6%, other work 2.0%, disk/compression 0.4%. Sample shares
are not isolated function durations and do not prove a single opcode bottleneck.
The profiled timing is excluded from performance scoring; its analysis is saved
in the JSON.

[Compressed guest counters and sampled IPs](office-divide-2026-10-27.profiles.zst)
retain both candidate workloads and pass a decompression round-trip check.
Raw traces/maps remain under `build/office-divide/`. Executable identities were
checked against the frozen files; selected sources match the build source
byte-for-byte. Clarifying an IR comment and rebuilding left both executable
hashes unchanged. The separate running VM has not been paused by this change.
