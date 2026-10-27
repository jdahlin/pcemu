# Word and Excel interpreter CPU cost, 2026-10-27

Diagnostic follow-up to [memory shifts and rotates](office-memory-shift-2026-10-27.md).
Immediate memory stores and memory ALU coverage are already included in this
candidate. This change measures the remaining interpreter work to select further
coverage. It adds no native instruction lowering.

The release executable is byte-identical to the previous candidate (SHA-256
`e579c5c004108dd56a2e5f88f7297dedcba4e3669a7ff7d63730cadd2aa2853d`).
The last matched original-baseline comparison therefore remains the performance
reference: Word CPU time 22.9% lower and Excel 9.1% lower. These are the previous
48-restore results, not new measurements with the diagnostic timers enabled.

## Method and validation

A profiling-only scope samples thread CPU time around CPU::step, including
snapshot, decoding, execution and fault delivery. An independent jittered interval
of 64–191 instructions avoids changing the existing IP sampler. Final decoded
leaf opcodes receive the timing; extended instructions do not count twice under
both 0F and their second byte. Nested scopes remain inclusive in their parent.
Unknown/fetch scopes and sampled faults/exceptions are reported separately.

Clock calibration subtracts the mean of 64 adjacent clock pairs after aggregating
samples. The observed pair means were 153–164 ns. Cheap instruction estimates are
sensitive to this correction, counter overhead and timer quantization. Estimated
shares rank broad groups; they are not release instruction latency or whole-task
CPU shares. Low sample counts and variation in interrupt services limit precision.
Thread CPU time excludes descheduled time, but does not remove contention or DVFS.
The separate GUI VM remained running; no permission to pause it was received.

Fifteen RAM-only C++ timing checks and six Python analyzer tests pass. Three
headless restores of Word loading C:\BENCH5M.DOC (4,807,680 bytes) and three of the
existing Excel dependent-recalculation fixture pass their task pixel gates. All
six profiles have exact equality between leaf attempts and interpreter attempts,
with no clock failures or nested scopes. Successful disposable overlays were
removed. Diagnostic elapsed times are not performance scores.

## Findings

The table reports estimated shares of interpreter CPU across three profiles.

| Remaining operation | Word | Excel | Interpretation |
| --- | ---: | ---: | --- |
| PUSHA + POPA | 3.63% | 8.43% | Useful shared ordinary-instruction coverage |
| MOV segment | 3.76% | 8.18% | Requires careful descriptor and fault semantics |
| ARPL | 5.96% | 7.01% | Every sampled instruction faults (54 Word, 140 Excel) |
| MOVSW/D | 4.58% | 2.89% | REP work needs separate treatment |
| OUT imm8,AL | — | 10.16% | Includes device work and sampled faults |

PUSHA/POPA shares are reasonably consistent across individual runs: Word
3.36–3.77%, Excel 8.08–8.66%. ARPL's high cost principally identifies fault work;
ordinary lowering alone would retain that cost. Word INT imm8 has too few samples
and too much run-to-run variation to infer a stable exact rank.

Already-supported MOV and ALU opcodes also remain in the interpreter. Their
presence does not establish missing lowering: cold memory, guarded replay, short
fragments and cache admission can all keep supported instructions interpreted.

The existing mapped Word Instruments trace contains five arena reset events
inside the measured task window (13 across the complete trace). The capacity
recycle path clears blocks and lookup/link state. Reset events alone do not prove
every reset's cause. A controlled cache-retention experiment is warranted before
assuming additional opcode coverage offers the largest total speedup. PUSHA/POPA
is the next ordinary coverage family indicated by these samples.

[Combined data, per-run accounting and cache events](office-interpreter-cost-2026-10-27.json)
and [compressed raw profiles](office-interpreter-cost-2026-10-27.profiles.zst).
The compressed payload was decompressed and compared byte-for-byte before saving.
