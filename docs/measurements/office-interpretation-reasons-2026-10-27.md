# Word and Excel interpreter attribution, 2026-10-27

The diagnostic build now attributes each interpreted opcode attempt to its JIT
execution context. This separates missing native coverage from warmup, deliberate
short-fragment admission, and guarded native replay. Normal builds use empty
inline hooks; this change makes no new release timing claim.

## Attribution

- `outsideJit`: a direct CPU step outside JIT dispatch.
- `forced`: JIT disabled, single-step/interrupt shadow, or fetch/metadata validation
  fault before a usable block was selected.
- `warming`: a block has not reached its normal translation threshold.
- `shortFragment`: supported short IR deliberately waits for hot promotion.
- `declined`: translation was attempted without producing a callable entry. This
  includes code-page decode fences and unsupported instructions; it does not
  mean the opcode itself lacks an implementation.
- `replay`: native execution stopped before the instruction and CPU::step replayed
  it, including cached-batch replay.

Scopes restore context on return and exceptions. Primary 0F and extended opcode
counts each receive the same reason; only primary counts belong in totals.
Sampled IP records also carry `primaryOpcode` and `interpretationReason` so hot
fallback locations can be distinguished. `lockPrefixAttempts` counts attempts
with a decoded LOCK prefix. The analyzer validates attribution sums and bounds,
aggregates multiple profiles, and accepts older profiles with missing fields.
Replay reasons identify the dispatch path, not individual memory guard failures.

## Existing fixtures

One current headless diagnostic restore per task passes its pixel gate and clean
exit. Word still opens the 4,807,680-byte C:\BENCH5M.DOC to its cover-title gate;
Excel still changes D13 and checks dependent recalculation. Existing snapshots
and small temporary overlays were reused. Library and other host work remained
active; these are diagnostic counts, not scored CPU comparisons.

| Reason | Word attempts | Word share | Excel attempts | Excel share |
| --- | ---: | ---: | ---: | ---: |
| Warming | 2,946,990 | 84.84% | 1,652,580 | 52.12% |
| Short fragment | 95,864 | 2.76% | 57,090 | 1.80% |
| Declined | 117,793 | 3.39% | 684,079 | 21.57% |
| Native replay | 237,140 | 6.83% | 384,642 | 12.13% |
| Forced | 75,696 | 2.18% | 392,624 | 12.38% |

Totals are 3,473,483 Word and 3,171,015 Excel interpreted attempts. No attempts
in these runs are attributed outside JIT dispatch. Two earlier diagnostic
iterations per app are retained in the raw profile archive.

## Concrete next native coverage target

CMP rm,r (`39`) has 19,933 declined attempts in Word and 472,216 in Excel.
Every sampled declined CMP in both tasks is at CS 0137, base zero,
IP **30707ffe**: 73 samples in Word and 1,860 in Excel. Its starting linear
page offset is ffe. An existing-snapshot restore inspected through an in-memory RAM pipe confirms
bytes `39 44 24 24`: `CMP [ESP+24h], EAX`. Its SIB and displacement are on the
next page, beyond the decoder's code-page fence. No RAM dump was saved to disk.
The existing native CMP implementation is therefore not enough to cover this
hot site. There are zero LOCK-prefixed CMP attempts, excluding the initial LOCK
hypothesis.

The next implementation should support this instruction across the code-page
boundary while validating both physical code pages, permissions and generations.
It must retain fetch-fault restart behavior and invalidate after changes to either
page. Broadly raising cold compilation would spend more time on Word's millions
of warming instructions and would not remove this decode fence.

Other missing families remain relevant: LES/LDS, ENTER and BSF/BSR. ARPL ranks
high in sampled interpreter cost, but all its CPU samples fault (31/31 Word,
40/40 Excel), before ModRM decode. Normal ARPL arithmetic would not remove that
observed cost. These observations guide scope; they do not establish a release
speedup.

## Validation and baseline

33 guest-profile checks pass, including actual forced/warming/declined/short
JIT dispatch, direct and cached watched-store replay, nested exception restoration
and LOCK attribution without leakage. Eight analyzer tests pass, including
invalid sums, negative values, LOCK bounds and old-profile compatibility.
Reference core passes 354,964 checks and bounded native batch passes 18,525.

The original-baseline results remain attributed to the prior release build:
[far-transfer report](office-far-transfer-2026-10-27.md). This diagnostic change
has no fresh unprofiled timing claim. The current analysis, task reports, source
hashes and validation logs are in [the JSON report](office-interpretation-reasons-2026-10-27.json);
all six raw diagnostic profiles are in
`office-interpretation-reasons-2026-10-27.profiles.zst`.
