# Local invariant snapshot: full variant not retained

Following the [context-tag experiment](context-cache-evaluation.md), this private
prototype copies invariant CPU values once after a batch's first native block
and uses them in the existing lookup/validation code. No data members are added
to Jit objects or cached blocks. Generation contents and current fetch permission
checks still execute on every entry; no validation predicate is removed.

A shared template body emits ordinary standalone and local-snapshot paths.
The snapshot contains execution/mode fields, CS identity, stack width, buffers,
bounds and the generation-array pointer. Interpreter/replay boundaries stop the
batch before another native entry can consume changed context. The batch budget
and device-polling granularity remain unchanged.

The existing 3,500 batch/timer checks and 759 experimental context mutation checks
pass. Three alternating headless pairs per application pass expected pixels, but
the measured CPU ratios (baseline/candidate) do not establish improvement:

| Task | Paired CPU ratios | Median |
| --- | --- | ---: |
| Excel calculation | 0.970, 0.890, 1.006 | 0.970 |
| Word warm launch | 0.992, 0.998, 0.922 | 0.992 |

Keep production unchanged. The measurements do not isolate snapshot copying,
register pressure, generated host code or shared-host variation as the cause.
A standalone RAM control and full OS gates are not run for this non-retained
variant. No new VM or Office fixture is created.

[Exact source/build/binary identity, tests, all six pairs and drivers](measurements/local-context-prototype-2026-10-26.json).
The narrower follow-up is to precompute only CS identity's lookup-key contribution
within native batches, leaving all validation values live. That follow-up is
separate from the full snapshot measured here.
