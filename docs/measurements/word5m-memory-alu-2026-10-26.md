# Word 97: immediate stores and memory ALU native coverage

The corrected build loads the same 4.8 MB document using **17.5% less median
process CPU than the original 57ce863 baseline**, and **8.8% less than the
previous 63a3151 build** in a fresh matched comparison. Native share of emulator
instruction counts rises from **90.8% to 92.7%**. Direct linking stays disabled.

## Added native coverage

- C6/C7 MOV immediate: byte/word/dword memory operands and register aliases.
- Memory-destination ADD, OR, ADC, SBB, AND, SUB and XOR with register or
  immediate sources, including sign-extended 83 immediates.
- Memory-source ADC/SBB with register destinations. Existing read-only ALU,
  CMP and TEST behavior remains covered.

LOCK-prefixed forms continue through the interpreter. This is the ordinary
binary ALU family, not a claim of complete x86 instruction coverage.

The lowering reuses the existing RAM/TLB/permission/code-bitmap guards. A fast
RAM hit performs the operation in generated ARM64. The guarded slow helper
captures the original RAM value, checks write translation, then writes the
result and reports code invalidation. The original value and source operand
feed the existing flag machinery; ADC/SBB capture incoming carry across helper
calls. A failed write cannot publish the new flags or partially commit data.
Devices and watched writes decline before callbacks, then replay in the
interpreter with exact prior registers, flags and instruction IP.

## Correctness edge case found and fixed

A directed test maps the operand onto its own page-table entry. Resolving the
write can set D in that same word. Reading the ALU input after the write probe
therefore computes a different result from the interpreter. The new regression
failed before the correction with:

> RMW observed its own write-probe dirty-bit update

The helper now captures the RAM input **before** the write probe, and publishes
it only after the probe succeeds. Tests cover all new RMW operations plus
existing memory INC/DEC. The correction also fixes that ordering for INC/DEC.

Earlier development timing runs are retained in the JSON but are not the final
performance scores. After correcting this edge case, all correctness suites and
the complete eight-round comparison were rerun on the corrected executable.

Two existing core fixtures also needed explicit HLT sentinels: their intended
32-instruction or single-MOV code had relied on zero-filled trailing RAM being
unsupported. Zero bytes now decode as a valid ADD memory instruction. The
fixtures now bound their intended programs explicitly; guest execution budgets
and opcode behavior were not restricted to preserve the old tests.

## Coverage diagnostics

Separate profile builds compare 63a3151 with the corrected candidate:

| Interpreted attempts | Prior | Corrected candidate |
| --- | ---: | ---: |
| All opcodes | 9,024,620 | 7,226,803 |
| C6 immediate byte stores | 174,088 | 6,395 |
| C7 immediate word/dword stores | 526,059 | 25,248 |
| 80 ALU immediate byte group | 195,715 | 33,251 |
| 81 ALU immediate group | 132,605 | 32,780 |
| 83 sign-extended ALU group | 625,633 | 165,428 |
| 01 ADD memory/register group | 297,700 | 6,073 |

Total interpreted attempts fall by about 20%. Native calls also fall from
22.37 to 21.29 million despite the higher native instruction share, consistent
with joining longer native fragments instead of merely adding more tiny calls.
Device polls fall from 9.52 to 7.99 million. Diagnostic guest instruction totals
remain similar (98.44 versus 98.75 million). These counts include faults and REP
host-batch resumptions; instrumented timings are not performance scores.

A separate corrected-build Instruments trace contains 1,652 task samples:
interpreter/MMU 25.8%, dispatch/cache validation 24.0%, generated code/helpers
21.2%, compilation 14.9%, capture/video/control 7.3%, device polling 4.4%.
Those percentages are sample shares, not isolated function CPU durations.
Raw traces/maps and full sampled guest IPs remain local in `build/word-memory/`.
The uncorrected and verification-overlapping diagnostic recordings are not used
for interpretation of the final build.

## Original-baseline comparison

All runs restore independently from the same immutable
`build/snapshots/word5m-ready`, launch `winword C:\BENCH5M.DOC`, and use the
unchanged original
[word5m-m1-2026-10-26.task.json](word5m-m1-2026-10-26.task.json).
The document is 4,807,680 bytes, SHA-256
`111edaf8471ba04a50c770fc5c5366b82013ec14558bc670633402c2b6fe5f7f`.
Readiness remains the original first-page cover-title pixels; this does not
measure completion of background pagination of every page.

Eight fresh holdout rounds rotate four labels: original-A, original-B,
previous 63a3151, corrected candidate. Both original labels use the identical
rebuilt 57ce863 executable. All 32 headless, unprofiled restores reach the
expected pixels and exit cleanly; no scores are discarded. The experiment
launches no overlapping builds/profilers during timed runs. Normal host
background activity remains, measured with rotating A/A controls.

| Build | Median process CPU | Median response | Native instruction share |
| --- | ---: | ---: | ---: |
| Original-A, 57ce863 | 1893.4 ms | 1943.5 ms | 85.6% |
| Original-B, identical binary | 1892.0 ms | 1941.0 ms | 85.6% |
| Previous 63a3151 | 1712.5 ms | 1765.3 ms | 90.8% |
| Corrected candidate | 1561.5 ms | 1611.2 ms | 92.7% |

The pooled original CPU median is 1892.1 ms: candidate CPU is 17.5% lower.
Versus the previous build's median, the reduction is 8.8%. Every matched round
beats both original controls and the previous build. Reductions versus the
mean of original A/B controls range from 16.8–18.6%; versus 63a3151, from
8.4–9.6%. Median absolute A/A difference is 0.32%, with a maximum of 1.47%.

Process user+system CPU includes capture, control/video and device work, and
excludes checkpoint restore, shutdown, blocked I/O and descheduled time. All
compilation costs are included. Scores are for this host/workload, not a
universal performance claim. Do not compare their absolute values with older
sessions: the original and prior builds were rerun in the same comparison.

## Tests and build provenance

A clean 63a3151 source archive plus only the x86 changes was built as Release,
excluding unrelated concurrent frontend/ARM changes. The core, bounded-batch
and cached-batch suites pass. The latter passes 201,700 checks; the bounded
suite passes 112,885 checks.

New interpreter comparisons cover operand/address width combinations, FS/SIB
addressing, high-byte register sources and destinations, immediate/register
forms, sign extension, carry clear/set, pending carry chains, cold TLB and
cached dirty-page paths, exact A/D updates, watched/MMIO callback ordering and
observable flags, read-only/cross-page faults with unchanged memory and prior
flags, self-modification that replaces the next native instruction, and
self-PTE aliasing. ADC/SBB loops run in basic/follow/trace modes with links
on/off. Timer tests require actual native memory-ADD execution while IRQs are
active, preserved stack/registers, exact ISR/EOI accounting, no early IRQ and
latency bounded by 127 instructions.

Eight host-profile accounting, two Instruments accounting and four debug-map
Python tests also pass. Source hashes, executable hashes, all timing records,
counters, the pre-fix regression result and test logs are preserved in
[the full report](word5m-memory-alu-2026-10-26.json).

- Original executable SHA-256:
  `1f2f4938d94f82cc9dc2852d00cd13b58d307a3fc1e69ef85c31d74772b57562`.
- Previous 63a3151 executable SHA-256:
  `8e3031397f135d9004f2f280f1371e79bbdabb58f39336ed98e9af07ffd3c745`.
- Corrected candidate SHA-256:
  `1ef64f45bdd52dd744add2ceab70bf2ea77c745ea437124c293ea96d98228ffe`.

```sh
python3 tools/bench_snapshot_task.py build/snapshots/word5m-ready \
  --task docs/measurements/word5m-m1-2026-10-26.task.json \
  --emulator build/word-memory/final/pcemu --repeat 1 \
  --restore-timeout 30 --output build/word5m-memory-recheck

ctest --test-dir build/word-memory/final \
  -R '^(tests|jit-batch-tests|jit-cached-batch-tests)$' --output-on-failure
```

Run tests separately from timing; rotate the four labels with fresh output
paths to reproduce the comparison. Memory bit tests remain a useful next
coverage target: 0F A3 still causes approximately 307,000 interpreted attempts.
