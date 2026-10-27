# Word and Excel: native instructions across a code-page boundary

The hot `CMP [ESP+24h],EAX` at 30707ffe now decodes and executes natively across
the code-page boundary. The decoder may finish a supported first instruction on
a second page; it returns immediately afterward. Instructions preceded by other
IR retain the existing page fence and can enter the new path on their next
separate dispatch. This also handles operand/address prefixes and 16-bit IP wrap.

## Fetch guards and restart behavior

Two-page native instructions own cold boundary metadata for the second fetch
IP, physical page and code generation. Their ordinary callable entry is null,
so cached tails and direct links cannot bypass validation. Before execution,
normal context, first-page translation/generation and embedded-memory assumptions
are checked, followed by the second physical page and generation. Writes or
remapping of either code page invalidate the entry. Callback-backed video code on either page and tails beyond RAM replay through
the reference fetch path, preserving device reads and the existing FF-byte behavior. Ordinary block headers,
cached-tail guards and direct-link records do not grow.

For a paged tail, both decoding and entry validation accept only a current
permitted warm TLB entry. A cold decode remains eligible for another translation
attempt after reference execution warms the fetch.
It does not page-walk a later instruction byte ahead of operand decoding. Cold
or denied translations replay through CPU::step, preserving original prefix IP,
fault priority, CR2 and page-table effects. In particular, C7 can fault on its
operand segment before fetching an immediate on the second page; a speculative
#PF must not replace that earlier #GP or alter CR2. A successful native instruction
returns for polling before executing its successor. Normal warmup admits the
expensive crossing instruction without the short-fragment delay.

The first version loaded cold boundary metadata on every ordinary dispatch.
The final version first checks the existing poll-boundary flag, limiting that
lookup to boundary blocks. The final version additionally excludes device-backed and beyond-RAM tails.
All three preliminary 48-run comparisons are retained alongside a 24-run
confirmation of the final cold-decode and device guards.

## Validation

Headless RAM-only suites pass:

- 4,760 cross-page checks: every split of 16/32-bit CMP forms, independent
  operand/address/code widths, real/paged execution with noncontiguous physical
  pages, basic/follow/trace modes, direct links enabled/disabled, repeated hot
  execution, normal warmup admission and the mandatory successor boundary.
- Changed code bytes and remapping on either page; absent first/second pages;
  CPL3 tail permission loss; segment-fault priority over a missing tail; 16-bit
  IP wrap with A20 enabled/disabled.
- Core 354,964; bounded batch 18,525; cached batch 20,456,574 with 103,944 linked
  transitions; flag liveness 102,230,994; far transfer 60,481; far return 89,428;
  guest-profile attribution 33.

Build: `build/office-cross-page/candidate`. Frozen binaries for the first
comparison are in `frozen`; intermediate binaries are in `optimized` and `verified`; the exact final binaries
are in `final`.

## Coverage

Both existing headless Office tasks pass their pixel gates and clean exits in
separate diagnostic restores. Word's declined CMP attempts fall from 19,933 to
zero; Excel's fall from 472,216 to zero. Current CMP attempts are 26,241 in Word
(26,227 warming, 2 short, 12 replay) and 13,836 in Excel (13,819 warming, 17 replay).
The earlier interpretation-reason report supplies the prior counts. These
profiles explain coverage; their timings are excluded from performance scoring.

## Original-baseline comparison

The final confirmation contains 24 unprofiled headless restores: four Word
rounds and two Excel rounds, each rotating original-A, original-B, prior and
candidate. Both original labels use the frozen `57ce863` executable. Prior is
release `f935b67`; the intervening `9997e82` added diagnostic attribution.
All task gates and clean exits pass. With existing user authorization, Library
process 2401 was suspended during each scored comparison and resumed afterward.
Other macOS work and CPU frequency were uncontrolled. No build, test or profiler
overlapped scored timings.

| Task | Original CPU ms | Prior CPU ms | Final CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M DOC | 1,894.42 | 1,352.50 | 1,363.55 | 28.02% | -0.82% |
| Excel recalculation | 7,299.41 | 6,333.90 | 6,329.00 | 13.29% | 0.08% |

Original is the pooled median of both original labels; other columns are
per-mode medians. CPU is process user+system time for all emulator threads
through the task interval, including control/capture work and excluding restore
and shutdown. Word response medians are 1,400.51 ms prior and 1,415.27 ms final;
Excel response medians are 6,342.94 ms prior and 6,337.83 ms final.

Word is slower in three of four pairs (from 0.55% better to 1.92% worse), a
small measured regression. Excel splits its two pairs (0.30% better, 0.15% worse);
its difference is inconclusive. Identical-original A/A differences reach 1.04%
for Word and 0.46% for Excel. The small final sample and uncontrolled host work
limit precision; no incremental speedup is claimed. Median native shares rise
from 96.494% to 96.537% in Word and 99.777% to 99.813% in Excel.

All 144 earlier scored restores remain in the report. Their three prototype
comparisons gave prior-relative CPU changes of -0.55%/-0.27%, -0.28%/+0.04%,
and -0.06%/+0.01% for Word/Excel respectively (positive means lower CPU).
These belong to their frozen binaries, not the final code. No completed scored
run was discarded or substituted.

Machine-readable task reports, executable/source hashes, checks and all 168
scored records are in [the report](office-cross-page-2026-10-27.json); six raw
coverage profiles are retained in `office-cross-page-2026-10-27.profiles.zst`.
 All measurements use existing snapshots and
small temporary compressed overlays. Word's 4,807,680-byte DOC gate checks its
cover title, not complete pagination. Excel checks the existing dependent
recalculation task. No large fixtures are recreated.

## Instruments and remaining work

Separate final-binary traces with timestamped JIT maps pass the same gates,
with Library running again; these runs are excluded from scoring. Word has
1,511 main-thread task samples: generated code/helpers 28.9%, dispatch/cache/
validation 22.3%, compilation 18.8%, interpreter 17.0%. Excel has 6,568 samples:
dispatch/cache/validation 46.3%, generated code/helpers 37.6%, interpreter 2.2%,
compilation 1.9%. The trace summaries and associated executable hashes are in
the JSON report.

Removing this measured decode fence improves coverage but has not established
an incremental speedup. Word's cold compilation/admission cost and Excel's
dispatch/validation cost remain substantial. LES/LDS and BSF/BSR still appear
among declined instruction families; further coverage work should continue to
separate warming, deliberate admission and guard replay from missing opcodes.
