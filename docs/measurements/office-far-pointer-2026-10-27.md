# Word and Excel: guarded native far-pointer loads, 2026-10-27

LES/LDS and LSS/LFS/LGS now use a guarded native backend. The latest preceding
profiles identify 31,491 declined attempts across this family in Word and
26,196 in Excel. This extends the existing segment-loader rules rather than
adding a separate descriptor implementation.

## Preflight and state

The helper computes the effective address from canonical GPRs, including
independent address width, SIB scaling, displacement and segment overrides.
Preceding dirty GPRs and flags are committed before the call. It captures the
offset and selector in their reference order, requiring warm permitted ordinary
RAM for each field. Separate fields may reside on different pages; an individual
cross-page field conservatively replays. A20 masking applies independently to
each access. Warm supervisor descriptor probes reuse `nativeSegment` and shared
segment decoding, including GDT/LDT, null/unusable selectors and real/V86 cases.

No state changes before all pointer reads and segment validation succeed. A
failure returns through the generated frame, then CPU::step supplies exact
fault rollback and callback ordering. The GPR is committed only after successful
segment validation; 16-bit loads preserve the upper half. Generated code reloads
that GPR before continuing, so following native memory operations see both its
new offset and the new segment cache. Existing memory metadata stores the
operands: ordinary block headers and memory-operand records do not grow.

LSS atomically replaces its segment and GPR and returns for polling, without
creating the interrupt shadow used by MOV/POP SS. The helper preserves the old
shadow while reusing the common segment loader. This matches the existing
reference and [Intel SDM volume 3A, section 6.8.3](https://cdrdv2-public.intel.com/812386/253668-sdm-vol-3a.pdf).
Following stack blocks revalidate the new SS.B before using cached code.
Non-SS loads can continue in cached and directly linked blocks.

## Validation

95,760 new headless RAM-only checks pass:

- All five instructions, eight GPR destinations, code/operand/address widths,
  real/protected/V86 modes, GDT/LDT, ring-3 supervisor descriptor reads,
  warm/cold paging, SIB/override sources and source-segment replacement.
- Null, nonpresent and invalid descriptors; selector-field page faults; complete
  destination rollback; flags before a fault even when a later ALU kills them.
- Separately warm fields on different pages; conservative crossing-field replay;
  exact MMIO read callbacks with visible flags/IP/instruction count/GPR state;
  A20 enabled/disabled.
- Dirty address GPRs, following native accesses through the newly loaded segment,
  changed SS.B with a cached PUSH, immediate LSS polling without a new shadow,
  multi-block cached execution and actual promoted direct-link transitions.

Other passing suites: core 354,964; bounded batch 18,525; cached batch 20,456,574
with 103,944 linked transitions; liveness 102,230,994; two-page fetch 4,760;
far transfer 60,481; guest profiling 33. Build: `build/office-far-pointer/candidate`;
frozen measured binaries: `build/office-far-pointer/final`.

## Coverage

Separate final-binary headless profiles pass the same task gates and clean exits.
Family interpreter attempts fall from 37,263 to 5,954 in Word (84.0%) and
30,530 to 4,325 in Excel (85.8%). Declined family attempts fall to zero in both;
remaining executions are primarily warming, with 133/16 native replays in
Word/Excel. These counts sum C4, C5, 0F B2/B4/B5 and exclude overlapping primary
0F totals. Prior counts belong to the two-page coverage build `52e037c`.
Diagnostic timings are excluded from scoring.

## Original-baseline comparison

All 48 unprofiled headless restores pass their task gates and clean exits:
eight Word rounds and four Excel rounds, each comparing identical original-A/B,
prior `52e037c`, and the frozen candidate. The first 24 and repeated 24 use the
same executable hashes; the repeat changes the rotating run order. All runs
are retained. The original executable remains the frozen `57ce863` baseline.

With existing user authorization, Library PID 2401 was SIGSTOP-suspended for
each scored set and SIGCONT-resumed afterward. Other host activity, including
display/audio services, and CPU frequency were uncontrolled. No builds, tests,
profilers or trace analysis overlapped scoring.

| Task | Original CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M DOC | 2,112.47 | 1,577.23 | 1,601.99 | 24.17% | -1.57% |
| Excel recalculation | 8,028.47 | 6,966.66 | 6,981.25 | 13.04% | -0.21% |

Original is the pooled median of both identical-original labels; remaining
columns are per-mode medians of process user+system CPU during the task. CPU
includes all emulator threads and control/capture work, excluding restore and
shutdown. Word response medians are 1,634.12 ms prior and 1,660.37 ms candidate;
Excel response medians are 7,079.19 ms prior and 7,046.60 ms candidate.

Word wins four of eight pairs, with changes from 7.26% worse to 4.24% better.
Excel wins two of four pairs, ranging from 2.83% worse to 2.86% better. Identical
original A/A differences reach 7.23%/3.38% in Word/Excel. The first 24 had both
candidate medians about 2% higher; changed-order repeats did not establish a
consistent direction. The combined medians remain higher and are reported
without claiming an incremental speedup or attributing the difference to this
helper. Host variation and these sample sizes limit confidence.

Release native-share medians are 96.534% prior versus 96.565% candidate in Word,
and 99.823% versus 99.815% in Excel. Whole-task instruction mixes vary with the
readiness gate; the targeted opcode diagnostics and directed tests establish
this family's native coverage rather than a monotonic whole-task percentage.

Task reports, executable/source hashes, pause/resume evidence and validation
logs are in [the JSON report](office-far-pointer-2026-10-27.json). The two raw
profiles are archived in `office-far-pointer-2026-10-27.profiles.zst`.
 Existing snapshots and small temporary compressed overlays are
reused; no Office fixtures are recreated. Word opens the 4,807,680-byte DOC to
the cover-title gate, which does not establish full pagination. Excel checks the
existing dependent recalculation fixture.

## Instruments and remaining work

Separate final-binary Time Profiler traces with timestamped JIT maps pass the
same gates, with Library resumed. They ran between the two scoring sets; trace
analysis ran after the second set. Word has 1,644 main-thread task samples:
generated code/helpers 27.3%, dispatch/cache/validation 25.4%, compilation
19.2%, interpreter 12.9%. Excel has 7,284 samples: dispatch/cache/validation
43.2%, generated code/helpers 38.2%, interpreter 2.3%, compilation 1.7%.
FarPointer helper descendants occupy two Word samples (0.12%) and one Excel
sample (0.014%), too few to resolve small helper costs reliably.

This removes another shared missing family without establishing a speed gain.
Word's compilation/admission and remaining ENTER/bit-scan fallbacks, plus Excel's
dispatch/validation, remain useful targets. Retain the distinction between
warming, declined translation and guarded replay when choosing the next change.
