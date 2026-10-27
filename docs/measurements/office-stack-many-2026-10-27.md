# Native PUSHA/POPA coverage, 2026-10-27

Following the [sampled interpreter cost study](office-interpreter-cost-2026-10-27.md),
PUSHA/POPA now lower to native ARM64 on guarded ordinary RAM. The study identified
them as a shared Word/Excel candidate: about 3.6% and 8.4% of estimated interpreter
CPU, respectively. Those percentages were diagnostic estimates, not predicted
whole-task speedups.

## Semantics and validation

The decoder represents the operation as a full 16- or 32-byte stack span, using
operand size independently of SS.B. Generated code retains all guest registers,
guards the complete span, and then executes the accesses without a helper.
PUSHA saves the original operand-sized SP/ESP. POPA skips restoring its saved
slot; word operands preserve upper register halves. Neither operation changes
flags, including deferred arithmetic flags from preceding native instructions.

The fast path requires one page of ordinary RAM and an existing valid TLB entry
when paging is enabled. Stores also require the existing writable/dirty and
code-bitmap checks. Both bitmap words are checked for an unaligned span. Watched
writes, VGA/device memory, unusable protected segments, stack wrapping, page
crossings, cold/dirty translations and possible code writes replay before the
first access. Thus faults and partial stack/register updates keep the existing
interpreter's ordering. Fast-path accesses have no guest-observable callbacks or
faults, so their grouped execution can safely commit SP after the accesses.

Validation is RAM-only and headless:

- 4,724 new stack-many checks cover operand/code/stack widths, real/protected/VM
  modes, paging, cold and warm translations, unaligned and wrapped stacks, partial
  page faults, devices, watched writes, code in the second bitmap word, live
  flags, repeated cached batches and direct links.
- Existing stack tests pass 43,600 checks; bounded batch tests pass 112,885.
- Existing cached tests pass 20,421,602 checks with 103,944 linked transitions.
- Both profiling restores and all 48 unprofiled workload restores pass their
  task pixel gates and exit cleanly. Successful private overlays are removed.

Build/test commands use `build/office-stack-many/candidate` and targets
`pcemu`, `pcemu-profile`, `jit-stack-many-tests`, `jit-stack-tests`,
`jit-batch-tests`, and `jit-cached-batch-tests`.

## Coverage

Per-run interpreter attempts, prior three-run average versus one candidate profile:

| Task | Operation | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | PUSHA | 63,951 | 4,092 |
| Word | POPA | 63,949 | 4,131 |
| Excel | PUSHA | 245,789 | 7,792 |
| Excel | POPA | 245,790 | 7,956 |

Together these fall by approximately 94% in Word and 97% in Excel. Remaining
attempts include guarded replay and cache/admission boundaries. Median release
native shares reach 94.58% for Word and 99.462% for Excel. Coverage is measured
separately from release timing; the profiling clock overhead is excluded below.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each run four executables:
original-A, original-B (the identical frozen 57ce863 binary), prior memory-shift
candidate, and this candidate. Every run is retained. Fixtures and task hashes
match within each workload; no builds, tests or profilers overlap these timings.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,898.665 | 1,458.843 | 1,446.094 | 23.84% | 0.87% |
| Excel dependent recalculation | 7,268.921 | 6,615.919 | 6,563.436 | 9.71% | 0.79% |

Word's paired candidate/prior median improvement is only 0.29%, with four wins
in eight rounds; A/A median difference is 0.51% and maximum 0.89%. Treat Word's
incremental timing as inconclusive. Excel wins all four paired rounds, with a
0.90% paired median improvement; A/A median difference is 0.18% and maximum
1.13%. Excel's small favorable result is consistent across the pairs, but remains
subject to host noise. Original-baseline improvements include all earlier work;
they must not be attributed to PUSHA/POPA alone.

A separate GUI VM remained active, along with macOS media analysis and indexing.
Process CPU time excludes descheduled time but includes all emulator threads,
control/capture and video work. Host contention and DVFS remain limitations.
Word's gate measures the rendered cover title of the existing 4,807,680-byte DOC,
not completed pagination. Existing Word/Excel snapshots were reused without
rebuilding Office fixtures or large disks.

[Machine-readable runs, counters, source hashes and test logs](office-stack-many-2026-10-27.json)
and [compressed raw diagnostic profiles](office-stack-many-2026-10-27.profiles.zst).
The compressed payload passes a byte-for-byte decompression round trip. Binary
hashes were rechecked against every timed record before generating the report.
