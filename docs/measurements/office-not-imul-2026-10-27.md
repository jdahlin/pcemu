# Word and Excel: native NOT and memory IMUL

NOT now lowers natively for byte/word/dword registers and memory, including high
bytes. Memory IMUL supports the two-operand 0F AF and immediate 69/6B forms with
16/32-bit operands. The preceding profile showed about 189,000 interpreted
memory IMUL operands in Word and frequent unary NOT in both applications.

Register and memory multiply forms share the signed product, truncation and
CF/OF overflow logic. Memory multiply materializes its prior flags, reads through
the guarded RAM/helper path, then commits the destination. It saves the complete
signed 64-bit product in the existing flag-spill slots and computes CF/OF only
when needed by a flag consumer, exit or replay. A following full flag writer can
discard those deferred flags. Undefined flags retain the interpreter's exact prior values. Immediate forms keep the
original upper destination half for 16-bit results, including when that register
also participates in the effective address. Memory addressing is captured before
the destination changes. Sign-extended 6B immediates use the existing decoder
convention.

NOT preserves flags, including deferred arithmetic flags from earlier native
instructions. Its RAM RMW path captures the input before write translation,
so setting D in an operand that aliases its own PTE cannot alter the result.
Watched/device writes and crossing-page operations replay through the interpreter.
Successful code writes exit through the completed instruction. Explicit LOCK
prefixes retain the interpreter path. Register NOT has no memory helper.

## Correctness

Core and bounded-batch suites pass 354,964 and 112,885 checks. Cached validation
passes **9,469,038 checks** with links on/off and 100,376 observed linked
transitions. The existing exhaustive rotate checks remain part of that total.
New comparisons cover NOT registers and high-byte aliases, operand/code/address
width combinations, FS/SIB addressing, cold and warm dirty-page paths, two- and
three-operand memory multiply, signed boundaries and overflow, sign-extended
immediates, exact preserved flags and upper halves, destination/address aliases,
actual native execution, self-PTE NOT ordering, self-modifying code, watched
MMIO callback order/prior register/flag images, read-only/not-present/cross-page
faults with unchanged destinations and data, and native linked loops in all
three block modes. Deferred carry is checked across NOT AH and a later ADC.
Deferred-product checks cover later memory reads, NOT, code-changing exits and
MMIO replay with newly changed CF/OF, plus a linked/block boundary into ADC. The shared multiplication refactor also
passes all existing register tests.

## Application protocol

The immutable Word 4,807,680-byte document checkpoint, original task and title
pixel gate remain unchanged, as do the Excel dependent-recalculation task and
checkpoint. Original-A/B use the identical 57ce863 executable. Prior is the
frozen 24a24e3 rotate/sign-extension build; the candidate is built from a clean
archive plus the selected x86 changes. Runs are headless with direct links off,
default jump following, and small disposable compressed disk deltas. No builds
or profilers overlap timing. Compilation, guest, device, capture/control and all
process-thread CPU are included; restore, shutdown, blocked and descheduled time
are excluded. Profiles are separate diagnostics, whose timings are not scores.
Word readiness is first-page rendering, not full background pagination.

## Development measurement and deferred flags

The first implementation computed memory IMUL's CF/OF eagerly after every read.
Its initial 48-run matched comparison showed the coverage improvement but a
small numerical Word timing regression within host variation. That prompted
review of the generated work rather than establishing a reliable slowdown.
Those records and the frozen executable remain
available; they are development results, not the final score. The revised
lowering uses the existing flag-spill area for the complete 64-bit product and
materializes CF/OF at a consumer or exit. Following full flag writers can drop
that work. Fault/replay paths retain the pre-access state, and successful
code-changing exits expose the completed multiply's exact flags. New tests
specifically check these lifetimes, including CF/OF changing from clear to set.

The final comparison includes five modes: original-A, original-B, the previous
24a24e3 build, the frozen eager prototype, and the deferred candidate. This
retains a direct comparison of both implementations alongside the original
baseline. Only the final matched comparison defines the published scores.

## Final matched results

All **60 unprofiled restores** pass the pixel gate and exit cleanly: eight
rotating Word rounds and four Excel rounds, each with all five modes. No final
scores are discarded.

| Workload | Original-A CPU | Original-B CPU | Prior CPU | Eager CPU | Deferred CPU | Prior → deferred native share |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Word | 1898.0 ms | 1898.1 ms | 1505.5 ms | 1484.2 ms | 1485.7 ms | 93.87% → 94.12% |
| Excel | 7470.1 ms | 7321.3 ms | 6641.0 ms | 6653.9 ms | 6634.0 ms | 99.41% → 99.43% |

The complete deferred candidate's median process CPU is **21.7% below the pooled
original Word baseline** and **9.7% below the pooled original Excel baseline**.
It beats both original controls in every final round. Scores are compared only
within this fresh experiment, not across older sessions' absolute timings.

Relative to the previous rotate/sign-extension build, Word CPU is 1.3% lower
and wins seven of eight paired rounds. Its median absolute A/A difference is
0.90%, maximum 1.98%. Excel CPU is only 0.11% lower, with mixed paired results;
its A/A median is 1.80%, maximum 7.62%. Excel's incremental timing is neutral
at this experiment's resolution. Coverage is the firmer conclusion.

Deferred versus eager median CPU differs by +0.10% in Word and -0.30% in Excel,
well within host variation. The direct comparison does **not** establish a
speedup from deferring IMUL flags, nor a reliable slowdown. It removes known
unconditional flag work when later instructions overwrite the flags and follows
the existing ALU flag-lifetime model, but that is not a measured global timing
win. Both implementations' native shares are essentially equal. The initial
48-run development result is retained separately and does not replace these
final comparisons.

## Diagnostic coverage

| Interpreted attempts | Prior Word | Deferred Word | Prior Excel | Deferred Excel |
| --- | ---: | ---: | ---: | ---: |
| F6 /2 byte NOT | 37,570 | 657 | 131,615 | 697 |
| F7 /2 word/dword NOT | 22,408 | 1,559 | 73,521 | 1,101 |
| 0F AF memory operands | 189,473 | 743 | 1,708 | 423 |
| All interpreted attempts | 6,081,828 | 5,797,750 | 8,387,685 | 8,200,273 |

The immediate memory IMUL forms are exercised by directed native tests; these
application tasks use them infrequently. Group /2 counts combine register and
memory operands. Remaining supported forms can still fail admission or replay.
Counts include faults and REP host-batch resumptions, and scheduling can alter
guest totals. They are not host CPU time attribution.

The remaining profiles still show integer DIV and memory shift work in Word;
these are candidates for the next native-coverage pass. The broader objective
remains active.

[Full records, source hashes, counters and test logs](office-not-imul-2026-10-27.json)
retain all 60 final results and the 48 development records, along with their
separate executable identities. Timings from profilers are excluded.

A separate final Word Instruments recording, with validated timestamped JIT
maps, contains 1,604 main-thread task samples: dispatch/cache validation 24.1%,
generated code/helpers 22.8%, interpreter/MMU 20.8%, compilation 17.0%, capture/
video/control 7.5%, polling/interrupts 4.7%, other work 2.5%, disk/compression
0.5%. These sample shares do not establish isolated function CPU durations or
an individual dominant guest opcode. The recording's timing is excluded from
the unprofiled comparison. Its analysis is retained in the JSON.

[Compressed guest counters and sampled IPs](office-not-imul-2026-10-27.profiles.zst)
retain both eager and deferred application profiles and pass a decompression
round-trip check. Raw traces/maps remain local under `build/office-invert/`.
All final/development executable identities were checked against their frozen
files. The final selected sources match `final-source/` byte-for-byte; separate
eager source hashes are saved with the development records.
