# Word and Excel: native LEAVE

LEAVE now lowers to the guarded native stack-read path. The preceding task
profiles showed 214,492 interpreted attempts in Word and 160,511 in Excel.
Keeping this epilogue instruction native lets adjoining native code continue
through frame teardown and RET without an interpreter boundary.

SS.B selects the width of the frame address and stack-pointer update;
operand size independently selects the saved BP/EBP width. Generated code
reads SS:BP/EBP, computes the post-pop SP/ESP from the original frame pointer,
then commits the loaded BP/EBP. Partial-register inserts preserve the unrelated
upper halves in mixed-width cases. Address-size and segment override prefixes
do not redirect LEAVE to a different stack. Flags remain unchanged.

Guest SP/BP commit only after a successful ordinary RAM read. Unusable segments,
page faults, crossing-page accesses and device reads use the guarded helper or
interpreter replay. Device callbacks therefore observe the interpreter's
intermediate SP image exactly, and a fault restarts with the original SP/BP.
SS.B is included in the existing stack-using block cache validation.

## Correctness and reproducibility

Core and bounded batch tests pass 354,964 and 112,885 checks. The cached-batch
suite passes 241,664 checks with linking on/off and 94,121 observed linked
transitions. New interpreter comparisons cover independent code/stack/operand
widths, address/segment prefixes, nonzero SS bases, 16-bit stack wrap, reads
straddling that boundary, preservation of upper register halves, pending flags,
MMIO callback order and intermediate SP/flags, unusable SS, not-present and
cross-page page faults, actual native execution, linked loops in basic/follow/
trace modes, LEAVE+RET, and cached SS.B changes between calls. An unused helper
left in the preceding bit-string test was removed; the final build has no
compiler warnings.

Comparisons restore the same immutable Word 4,807,680-byte document checkpoint
and Excel dependent-recalculation checkpoint used by the preceding reports.
The original executable is still 57ce863; the prior executable is the frozen
5c0420e memory-bit-string build. Runs are headless with linking disabled, default
jump following, and disposable compressed disk deltas. No build or profiler
runs overlap timed comparisons. The two original labels use identical binaries.
Word readiness means its original first-page title gate, not full background
pagination. Process CPU includes all guest, compilation, device, capture and
control work; it excludes restore, shutdown and descheduled/blocked time.

## Matched baseline comparison

All **48 unprofiled restores** reach the expected pixels and exit cleanly:
eight rotating rounds for Word and four for Excel, with original-A, original-B,
prior bit-string build and candidate. No scores are discarded.

| Workload | Original-A CPU | Original-B CPU | Prior CPU | Candidate CPU | Prior → candidate native share |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word | 1966.3 ms | 2028.8 ms | 1613.0 ms | 1610.9 ms | 93.25% → 93.45% |
| Excel | 7549.7 ms | 7588.5 ms | 6929.5 ms | 6924.2 ms | 99.35% → 99.36% |

The complete candidate uses **19.9% less median CPU than the pooled original
Word baseline** and **8.6% less than the pooled original Excel baseline**. It
beats both original controls in every round. These are fresh matched results;
older sessions' absolute scores are not compared or attributed to LEAVE.

LEAVE itself establishes a coverage gain, **not a measured speedup**. Candidate
versus prior median CPU differs by only -0.13% in Word and -0.08% in Excel.
Median absolute A/A differences are 2.58% (Word) and 0.53% (Excel), with maxima
6.55% and 2.30%. Word wins five of eight paired prior comparisons; Excel wins
three of four, including one effectively tied result. Paired differences range
from -1.97% to +7.10% CPU reduction in Word and -5.35% to +4.74% in Excel.
The experiment does not resolve an incremental performance change.

Separate diagnostic profiles show:

| Interpreted attempts | Prior Word | Candidate Word | Prior Excel | Candidate Excel |
| --- | ---: | ---: | ---: | ---: |
| C9 LEAVE | 214,492 | 27,626 | 160,511 | 14,738 |
| All opcodes | 6,663,121 | 6,452,211 | 9,336,580 | 9,220,231 |

Remaining attempts can fail native admission or require replay. Counters include
fault attempts and REP host-batch resumptions; scheduling can alter guest totals.
They measure coverage, not the host CPU cost of each opcode.

The new profiles still show shared CWD/CDQ and rotate gaps. CWD/CDQ has 85,143
Word and 221,278 Excel attempts. Register rotates remain common, including byte
RCL (97,858 Word group attempts, 64,890 Excel) and immediate ROL (14,041 Word,
205,097 Excel). Those group totals include both operand classes and should be
read alongside memory/register counters. Word also retains memory IMUL and DIV
work. These are candidates for the continuing coverage work.

[Full timing records, selected source hashes, counters and test logs](office-leave-2026-10-27.json)
are saved alongside this report. Raw builds, traces and VM disks remain local.

A separate Word Instruments recording, with validated timestamped JIT maps,
contains 1,798 main-thread task samples: dispatch/cache validation 24.2%,
interpreter/MMU 22.2%, generated code/helpers 21.4%, compilation 17.0%, capture/
video/control 7.5%, polling/interrupts 4.7%, other work 2.6%, and disk/compression
0.4%. These sample shares continue to show several material costs and do not
prove that an individual remaining guest opcode dominates host time. The
recording's timing is excluded from the unprofiled comparison.

The full JSON retains the Instruments analysis. Raw traces/maps remain local
under `build/office-leave/`. [Compressed guest counters/IP samples](office-leave-2026-10-27.profiles.zst)
are saved for both candidate workloads and verified by decompression round-trip.
Prior profiles remain with the preceding bit-string report.
