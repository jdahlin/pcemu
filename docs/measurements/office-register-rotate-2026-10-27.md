# Word and Excel: native register rotates and sign extension

Register ROL/ROR/RCL/RCR now lower to ARM64 for byte/word/dword operands and
implicit-one, immediate and CL counts. CBW/CWDE/CWD/CDQ also lower natively.
The preceding profiles showed 85,143 CWD/CDQ attempts in Word and 221,278 in
Excel, plus substantial register-rotate activity in both workloads.

Counts are masked to five bits, then normalized at operand width for ROL/ROR
and at width+carry for byte/word RCL/RCR, matching the interpreter. Zero counts
preserve the complete operand and flags. Generated code uses a 64-bit ring for
all widths, including the 33-bit carry ring for dword operands. It captures CL
before modifying an aliased destination and supports AH/CH/DH/BH. Only CF and,
for normalized count one, OF change. All other flags retain their exact prior
values, including the interpreter's preserved undefined flags. Liveness
conservatively retains the complete incoming arithmetic flag image for rotates.

Sign extension changes only the required accumulator/result register width,
preserves upper halves in 16-bit forms, and leaves pending arithmetic flags
intact. The generated-code map identifies both new IR kinds. Memory rotate and
shift forms retain their existing interpreter path.

## Correctness

The core and bounded-batch suites pass 354,964 and 112,885 checks. The cached
suite passes **9,425,162 checks**, with direct links on/off and 95,937 observed
linked transitions. Small RAM-only programs reuse their native compilation to
compare all 256 byte values and 32 masked counts for every rotate operation,
immediate/CL forms, four destination encodings (including high bytes and CL/CH
aliases), and four flag images covering independent carry/overflow combinations
and different preserved flags. CL aliases retain the input's high three count
bits and respect the shared low count/destination bits. Wider forms
exercise boundary values and exact upper-register preservation. These programs
require native execution rather than silently passing through interpreter replay.

Additional comparisons cover every register encoding with implicit-one counts,
16/32-bit code defaults, counts through 255, normalized-zero and full-ring
counts, pending flags before rotates, subsequent carry consumers, linked loops
in basic/follow/trace modes, and positive/negative sign-extension boundaries.
Sign-extension results are saved before LAHF so AH overwrites cannot hide a
CBW error. The test workload uses small in-memory CPUs and no guest disks.

## Application measurement scope

Application checks retain the same immutable Word 4,807,680-byte document and
Excel dependent-recalculation checkpoints, original task hashes and pixel gates.
The original executable is 57ce863; the prior build is the frozen c7dad34 LEAVE
build. Timed runs are headless, linking disabled, default jump following, with
private compressed deltas removed on success. No builds or profilers overlap
the comparisons. Original-A and original-B use the identical binary. Profiles
are separate diagnostics and their timings are excluded. Word's gate measures
first-page title rendering, not completion of background pagination. Process
CPU includes compilation, devices, control/capture and all process threads,
while excluding restore, shutdown, blocked time and descheduled time.

## Coverage diagnostics

| Interpreted attempts | Prior Word | Candidate Word | Prior Excel | Candidate Excel |
| --- | ---: | ---: | ---: | ---: |
| 98 CBW/CWDE | 14,130 | 1,551 | 16,668 | 927 |
| 99 CWD/CDQ | 85,143 | 4,253 | 221,278 | 2,443 |
| C0 byte immediate group | 55,321 | 2,227 | 9,013 | 2,470 |
| C1 immediate group | 182,844 | 148,680 | 255,525 | 28,830 |
| D0 byte implicit-one group | 159,520 | 6,016 | 227,460 | 3,320 |
| D1 implicit-one group | 39,127 | 14,704 | 123,410 | 4,127 |
| D2 byte CL group | 9,352 | 659 | 1,756 | 276 |
| D3 CL group | 62,724 | 15,838 | 3,167 | 1,507 |
| All interpreted attempts | 6,452,211 | 6,081,828 | 9,220,231 | 8,387,685 |

Shift/rotate group counts include unchanged memory operands and shifts, not just
newly supported register rotates. For example, 118,466 candidate Word C1 attempts
have memory operands. Remaining supported register forms may also fail native
admission or replay. Diagnostic counts include faults and REP host-batch
resumptions, and guest totals can vary with timer scheduling. They measure
coverage, not attribution of host time to an opcode.

## Matched baseline results

All **48 unprofiled independent restores** pass their task pixel gate and exit
cleanly: eight rotating rounds for Word and four for Excel. No scores are
discarded.

| Workload | Original-A CPU | Original-B CPU | Prior CPU | Candidate CPU | Prior → candidate native share |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word | 1927.1 ms | 1916.6 ms | 1539.9 ms | 1516.4 ms | 93.44% → 93.88% |
| Excel | 7439.7 ms | 7477.4 ms | 7020.2 ms | 6844.5 ms | 99.35% → 99.41% |

The complete candidate's median process CPU is **20.9% below the pooled original
Word baseline** and **8.0% below the pooled original Excel baseline**. It beats
both original controls in every round. Original controls are rerun here; older
sessions' absolute scores are not compared or attributed to this change.

Relative to the prior LEAVE build, median CPU is 1.5% lower in Word and 2.5%
lower in Excel. Word wins seven of eight paired prior comparisons, with a median
absolute A/A difference of 0.84% and maximum 7.21%. One prior Word measurement is
relatively slow, producing an 11.15% paired reduction; it is retained alongside
the much smaller 0.53–1.99% gains in the other winning rounds and one -0.21%
result. Excel's paired differences are +0.08%, -0.01%, +6.62%, and +0.70%; its
A/A median is 0.33%, maximum 2.11%. Excel's median reduction is influenced by one
large paired difference and only four rounds; a precise incremental speedup
magnitude is not established. Coverage gains are clearer than the small timing
differences.

The next profiles still show memory IMUL and DIV work in Word and unary NOT in
both workloads. Shared register arithmetic coverage has improved substantially,
but these remaining missing forms keep the broader coverage objective active.

[Complete timing records, source hashes, counters and test logs](office-register-rotate-2026-10-27.json)
are retained with this report. Build products, application images and trace
bundles remain local and ignored by Git.

A separate Word Instruments recording with validated timestamped JIT maps has
1,629 main-thread task samples: dispatch/cache validation 24.3%, generated
code/helpers 22.8%, interpreter/MMU 19.5%, compilation 18.3%, capture/video/control
7.4%, polling/interrupts 5.3%, other work 2.2%, and disk/compression 0.2%. These
sample shares identify several material costs; they do not establish isolated
function durations or a single dominant guest opcode. The trace's timing is
excluded from performance scoring. Its analysis is retained in the full JSON.

[Compressed candidate guest counters and sampled IPs](office-register-rotate-2026-10-27.profiles.zst)
are saved for both workloads and verified by decompression round-trip. Raw trace
bundles/maps remain in `build/office-rotate/`. All timing records were checked
against the current frozen executable hashes, identical original controls, one
task hash per workload, and headless/unprofiled/unlinked settings. Selected
source files also match the frozen build source byte-for-byte.
