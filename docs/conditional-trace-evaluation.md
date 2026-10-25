# Conditional traces: useful-work experiment

The current host profile attributes 27.1% of Excel task samples to dispatch and
16.9% to device polling, with only 3.88 guest instructions per dispatch. This
experiment tests the existing `--jit-traces` mode against `--jit-follow-jumps`
using the same frozen executable (`1c5a879` x86 source set), snapshots, input and
pixel-verified outputs. No runtime code or default mode is changed.

## Loaded Excel calculation

Three alternating pairs complete the same dependent recalculation:

| Pair | FollowJumps process CPU ms | Trace process CPU ms | Baseline/trace ratio |
| --- | ---: | ---: | ---: |
| 1 | 12944 | 11591 | 1.117 |
| 2 | 13323 | 9384 | 1.420 |
| 3 | 11881 | 9858 | 1.205 |

All three favor traces. Median paired process CPU ratio is 1.205, about 17.0%
less CPU; response ratio is 1.214 and execution-wall ratio 1.209. Shared-host
variation is substantial, so this is exploratory application evidence, not an
exact expected gain for all Win95 work. Guest instruction deltas are small and
all expected result pixels match. Profile-inclusive timings are not used here.

A separate instrumented trace run explains the mechanism. Compared with the
matched-source FollowJumps counter run, roughly the same 1.404 billion guest
instructions require 251.24 million dispatches instead of 362.34 million:
**30.7% fewer dispatches**, and 5.59 rather than 3.88 guest instructions per
dispatch. Device polling tracks dispatch count. Interpreter attempts fall from
22.53 million to 20.56 million. Translation attempts rise from 58,087 to 63,690.
The runs are diagnostic counts, not instruction-by-instruction lockstep traces.

## Word warm launch

Three alternating Word launch pairs all reach the same rendered Page 1 / Sec 1
status. CPU ratios are 0.882, 1.364 and 0.979; response ratios are 0.897, 1.378
and 0.974. There is no consistent gain. The median is slightly unfavorable,
with broad host/run variation. Trace runs compile roughly 40,000 blocks versus
36,000–37,000 for FollowJumps. This is consistent with a larger cold translation
footprint, but block counts alone do not assign its CPU cost.

## Correctness and decision

The frozen trace-mode executable passes 17,861 indirect-jump checks, plus
headless DOS 6.22, Win3.11 Notepad/save/DOS readback, and both rtts Win95 and
installed OSR2 Calculator/clipboard/Notepad/restart/exact-file checks. State
metadata independently confirms trace mode in all four OS tests. Successful
private compressed disks and their orphan index sidecars are removed.

The rtts verifier initially misses a visibly correct Calculator title. It leaves
the guest alive. After adding a second reading of actual bright glyphs in blue
title bands, the same guest resumes through applications/restart and exact
38-byte stopped-guest FAT readback. This is a resumed verification, not an
uninterrupted checker pass. See [title OCR evidence](win95-title-ocr.md).

Keep traces experimental. Fresh OSR2 Setup has not been repeated in this mode,
and the Word launch tradeoff does not justify switching every workload. The
next architectural candidate is selective promotion of hot blocks to larger
traces while retaining cheap compilation for cold code. That adaptive policy
is not implemented or claimed by this experiment. The twofold stretch target
remains unachieved.

[All paired measurements, counter data, driver sources, exact build identity and OS gates](measurements/conditional-traces-2026-10-26.json).
