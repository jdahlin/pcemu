# Selective hot conditional traces: rejected prototype

The [Office host profiles](office-current-profiling.md) identify frequent JIT
dispatch and board polling as major costs in loaded Excel. [Global conditional
traces](conditional-trace-evaluation.md) reduce dispatches there, while Word
launch does not consistently benefit. This prototype promoted only hot native
conditional exits, retaining FollowJumps compilation for cold code.

**No production runtime change is retained.** The complete proposed patch,
tests, build commands and measurements are preserved in the
[measurement record](measurements/adaptive-traces-rejected-2026-10-26.json).
The compressed patch has a verified byte-for-byte round trip.

## Proposed policy

`PCEMU_JIT_TRACE_AFTER=1024` opted FollowJumps callers into promotion after 1024
native entries. Unset/zero retained the old policy; explicit Basic/Trace modes
ignored the opt-in. Only native conditional exits accumulated heat. Changed
code, execution identity, invalidation and arena recycling reset heat;
interrupt-shadow fallback did not spend a native visit. Existing bounded trace
decoding and precise faults were reused. Machine snapshots did not retain JIT
code or hotness. Profile builds added a trace-promotion counter.

A constructor-selected function pointer chose specialized legacy/adaptive
execution bodies sharing one source. The legacy body omitted hotness
accounting, but the indirect call and changed layout were not assumed free.
Several earlier implementations and RAM control results are retained, including
unfavorable results; they are not additional independent application trials.

## Final application measurements

Three alternating headless restored Excel calculation pairs all favor the
opt-in: baseline/candidate process-CPU ratios are 1.218, 1.130 and 1.134
(median 1.134, about 11.8% less CPU); response median is 1.137. Every run
verifies the expected rendered sum. This suggests dispatch amortization can
help, but does not establish a general improvement.

Word warm-launch CPU ratios are 0.990, 0.932 and 0.981; response median is
0.966. There is no gain in this final implementation. These launches start with
warm OS caches and cold JIT state, not loaded document editing.

The final disabled-feature Excel control is unfavorable in every pair:
process-CPU ratios 0.820, 0.686 and 0.678. Response median is 0.647. Other chats
started ARM/HVF and x86 validation while these controls ran, alongside browser
load. Contention and heterogeneous-core placement mean these are not clean
estimates of implementation overhead, even for process CPU. Nevertheless,
there is no evidence here that the legacy path is acceptably unchanged.
Disabled-feature Word controls are mixed too (CPU ratios 1.001, 0.804, 1.094).
The candidate is therefore rejected. The favorable opt-in result is not a
shipped speedup.

## Correctness and limits

The prototype passes 30,185 differential adaptive checks covering flat/paged
16/32-bit code, thresholds, invalidation, interrupt shadows, arena recycling and
precise faults partway through promoted traces. Frozen matched-source core and
jump suites pass 354,948 and 17,861 checks. The adaptive suite also passes when
built against the current repository core through CMake/CTest. Seven sample
analyzer tests pass for the experimental frame names; the production analyzer
and its six passing tests are restored with the runtime.

No additional adaptive cold-boot, fresh Windows Setup or OS persistence gate is
claimed. Existing Office checkpoints are reused without reinstalling Office or
creating large disk fixtures. The broader twofold performance target remains
unachieved. The next useful work is stable dispatch/poll amortization with
controlled disabled-path comparisons, and attribution of emitted ARM64 code.
