# Native BSF/BSR coverage for Office

Five short Excel launch/load profiles exposed 10,568 declined BSF executions and
11,826 declined BSR executions in total. One Word profile had 3,081 and 61 respectively. These are
actual decoded extended-opcode attempts, not the overlapping primary 0F count.
The preceding Instruments profile put the short Excel workload at about 24%
dispatch, 23% generated code/helpers, 21% compilation and 17% interpreter.

The ARM64 JIT now lowers register and memory BSF/BSR for 16/32-bit operands.
BSF uses RBIT and CLZ; BSR uses CLZ and subtraction. Zero sources retain the
destination, matching the reference interpreter; only ZF changes. A 16-bit
destination retains its upper word. Sources are captured before aliased
destinations change. Other flags, including those architecturally undefined by
these instructions, retain the interpreter's values.

Memory forms reuse the guarded RAM read path and commit after a successful
read. Cold TLB entries, MMIO and cross-page accesses replay before access, so
callbacks and fault/page-table effects occur once. Prior lazy flags are
materialized before the access. Both scan kinds are admitted in short native
fragments after ordinary warmup, including singleton register scans. Debug maps
identify the register operation as BitScan.

## Correctness

The new target `jit-bit-scan-tests` passes 63,212 checks. It covers every GPR
source/destination alias, code default and operand width, single-bit positions
and zero, mixed-bit memory inputs, 16/32-bit addressing, FS override, cold/warm
paging, exact MMIO read counts, successful/faulting cross-page reads with a later
flag writer, and normal-warmup cached loops in all three block modes with links
enabled/disabled. Full GPRs, flags, IP, instruction count, CR2 and RAM are
compared with the interpreter.

Regression targets pass: core CPU 354,964 checks; flags 102,230,994; batch
18,525; cached batch 20,456,574; cross-page instruction fetch 4,760; guarded reads
255,535; profile timing 33. The guarded-read suite had a stale negative-cache
test using CLI, which already had native coverage before this change. That test
now uses interpreted SMSW AX, checks preservation of changing upper EAX values
without altering the JIT context, and still verifies
that replacing its code immediately invalidates the negative entry.

## Measurement scope

All Office runs are headless and use existing immutable snapshots with small
private compressed disk children. The Library VM is paused around scored runs
and resumed in a finally block. Release builds, tests, diagnostic profiles and
trace analysis do not overlap scored timing. Every scored run verifies the
existing rendered result gate, starts with a fresh JIT, and excludes VM restore.
Word includes application launch and the 5 MB DOC's cover; Excel load includes
launch, opening the 64-row/650-formula XLS, and its calculated summary; Excel
recalculation edits D1 in the already-open 4,096-row workbook. These scopes must
remain distinct. Process CPU includes emulator threads and control/video work;
elapsed time includes polling and scheduling as well.

The main comparison uses four rotated-order rounds of original `57ce863`, prior
`e5be4dd` and candidate for each workload. Each of those 36 batches invokes the
harness's default five independent restores: 180 verified runs in total. The
summary compares medians of the four batch medians, retaining every individual
sample in the JSON report. A separate short-Excel holdout rotates candidate
against two labels for the identical prior binary, with six single-run rounds.
That provides an A/A control while checking the apparent short-load regression.

## Results

All 198 scored restores passed. Times below are milliseconds. The last column
compares elapsed time with the original baseline, so includes earlier native
coverage work as well as this change.

| Workload | Original elapsed / CPU | Prior elapsed / CPU | Candidate elapsed / CPU | Below original elapsed |
| --- | ---: | ---: | ---: | ---: |
| Word launch + 5 MB DOC cover | 2,074 / 2,021 | 1,515 / 1,461 | 1,523 / 1,470 | 26.6% |
| Excel launch + formula XLS | 1,628 / 1,587 | 1,281 / 1,243 | 1,338 / 1,290 | 17.8% |
| Excel edit + recalculation | 7,944 / 7,910 | 7,164 / 7,084 | 7,082 / 7,035 | 10.8% |

No reliable incremental speedup or regression is established. In particular,
the apparent 4.4% short-load elapsed regression in the main cohort reversed in
the holdout: identical prior-A/prior-B controls measured 1,337/1,288 ms elapsed
and 1,295/1,247 ms CPU; candidate measured 1,228 ms elapsed / 1,190 ms CPU.
That control spread and reversal prevent attributing the main-cohort difference
to bit-scan lowering. All samples, including slower results, are retained.

Separate diagnostic profiles confirm the intended coverage change. Across five
short Excel loads, interpreted BSF/BSR attempts fell from 23,809 to 1,420
(94.0%); every remaining attempt is warming, with zero declines or replays.
One Word profile fell from 3,384 to 242 (92.8%), again warming only. The
candidate recalculation profile has 179 warming-only bit-scan attempts.
Each candidate workload was profiled five times; Word/recalculation comparisons
use run-0 against the previous run-0, while the short-load comparison aggregates
five profiles on both sides. Raw counters are retained in the adjacent zstd JSON
bundle. Profiled times are excluded from the scored comparison.

This is a measured native coverage improvement with no claimed incremental
performance win. Descriptor queries (notably LAR/LSL) and privileged 0F groups
remain among the new Excel load's declined instructions. Dispatch, compilation
and warming still matter; removing bit-scan fallbacks does not finish the wider
Word/Excel native coverage goal.
