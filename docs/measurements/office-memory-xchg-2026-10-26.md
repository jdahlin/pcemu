# Word and Excel: native memory XCHG

Memory XCHG (86/87, byte/word/dword) now uses the guarded native RAM path.
The new Excel profile selected this operation: the prior build interpreted
486,894 word/dword XCHG attempts, including 485,761 memory operands, during
its dependent recalculation task. It also occurs frequently in Word loading.

The operation captures both inputs before changing either destination. Only a
successful store commits the register result. Flags remain unchanged. Cold TLB,
permission/dirty-bit updates and code writes use the existing helper; devices,
watched stores and crossing-page accesses replay through the interpreter.
The helper reads before write translation, including operands aliasing their
own page-table entry. Explicit LOCK prefixes retain interpreter execution.
Execution follows the emulator's existing single-vCPU instruction model.

## Correctness

Core, bounded native batch and cached batch tests pass. New comparisons against
the interpreter run with direct linking on and off and cover paging, 16/32-bit
code and address sizes, all operand widths, FS/SIB addressing, address/source
register aliases, high-byte registers, exact flags, self-PTE dirty-bit ordering,
self-modifying code, watched MMIO callback order, and read-only/cross-page faults
without committing a register result or partial write. Separate assertions
require actual native XCHG execution, rather than only successful replay.

## Local fixtures and measurement scope

Word retains the original immutable 4,807,680-byte DOC checkpoint and pixel
gate. The Excel task changes D1 in the existing PERF4096.XLS workbook and checks
the recalculated SUM pixels. No application or guest fixture was recreated.
Excel's recovered checkpoint is copied locally using a 2.3 MB overlay over the
byte-verified local Word image. Both original child disks share the same parent;
the union of their 114 modified 4 KiB blocks is replaced with the logical Excel
contents and read back. Outside this union, both inherit identical parent data.
State, preview and task are copied unchanged from the recovered Excel snapshot.

Both workloads run headless with direct linking disabled and default jump
following. Profile timings are excluded from performance results. Unprofiled
runs compare the original 57ce863 executable, the prior ac04dfa-equivalent
memory-ALU executable, and the XCHG candidate. Two original labels use identical
executables to measure host variation. No builds or profilers run during timing.
Word readiness remains its first-page title, not full background pagination.

## Results

Eight rotating rounds per workload complete all **64 independent restores**
without a failed gate or discarded score. Median process CPU and native guest
instruction share are:

| Workload | Original-A | Original-B | Prior memory ALU | XCHG candidate |
| --- | ---: | ---: | ---: | ---: |
| Word CPU | 2177.6 ms | 2152.4 ms | 1805.7 ms | 1783.9 ms |
| Word native share | 85.64% | 85.65% | 92.69% | 92.85% |
| Excel CPU | 7925.7 ms | 7966.8 ms | 7436.4 ms | 7338.1 ms |
| Excel native share | 98.57% | 98.57% | 99.30% | 99.33% |

The candidate's CPU median is **17.4% below the pooled original Word baseline**
and **7.4% below the pooled original Excel baseline**. It beats both original
controls in every round. The numerical reductions versus the prior build are
1.2% for Word and 1.3% for Excel. Word wins only four of eight paired prior
comparisons; its median absolute A/A difference is 1.34%, with a maximum of
11.44%. This experiment does **not** establish a reliable incremental Word
speedup from XCHG. Excel wins seven of eight prior comparisons, with median A/A
variation 0.34% and maximum 2.74%; the apparent incremental gain is modest.
No absolute scores are compared with previous sessions' scores.

Separate diagnostic profiles establish the coverage effect:

| Interpreted attempts | Prior Word | XCHG Word | Prior Excel | XCHG Excel |
| --- | ---: | ---: | ---: | ---: |
| All opcodes | 7,226,803 | 7,017,898 | 10,046,353 | 9,519,563 |
| 87 word/dword XCHG | 140,592 | 6,191 | 486,894 | 4,094 |
| 86 byte XCHG | 58,080 | 55,259 | 19,013 | 4,881 |

Remaining attempts can still require replay or never reach native admission;
these data do not claim complete native execution of every XCHG. In particular,
Word's byte XCHG attempts remain common. Counts include fault attempts and REP
host-batch resumptions, and guest totals can vary with timer scheduling.

Word's next prominent missing forms include memory BT (307,269 memory attempts),
LEAVE (213,446), and memory IMUL (189,453). Excel has 80,301 memory BT attempts,
160,943 LEAVE, and 218,647 CWD/CDQ attempts. Memory bit tests and LEAVE therefore
have evidence in both workloads. Excel's largest remaining interpreted categories
also include CLI/STI and segment loads, which require more careful interrupt and
privilege-state handling than ordinary arithmetic lowering. Opcode counts guide
coverage selection; they are not host CPU time attribution.

Core tests pass 354,964 checks, bounded-batch tests 112,885, and cached-batch tests
203,152, including 85,041 observed direct-link transitions. Selected source hashes,
all 64 timing records, fixture provenance, profile counters and test logs are in
[the full measurement record](office-memory-xchg-2026-10-26.json).

A separate candidate Instruments recording, with lifetime-aware JIT maps, has
1,929 main-thread task samples: dispatch/cache validation 22.4%, interpreter/MMU
22.2%, generated code/helpers 22.2%, compilation 16.4%, capture/video/control
9.3%, polling/interrupts 5.0%, other host work 2.2%, and disk/compression 0.3%.
The leading three categories are effectively tied at this sampling resolution;
no single opcode is proven to dominate host time. Native coverage can reduce
interpreter exits and fragmentation, while dispatch and compilation remain
material costs. Raw traces/maps remain local under `build/office-coverage/`;
the full JSON retains the analysis and
[compressed guest counters/IP samples](office-memory-xchg-2026-10-26.profiles.zst)
retain both Excel profiles and the candidate Word profile. The compression was
round-trip verified. The prior Word profile is retained by the preceding memory
ALU report.
