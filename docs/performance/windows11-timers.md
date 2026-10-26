# Windows ARM64 synthetic timer investigation

The Hyper-V report's `timerActivity` object distinguishes configuration/count
writes, repeated values, zero-count disables, AutoEnable transitions, periodic
rearms, and writes that leave an already-armed absolute deadline unchanged.
The sixteen `configFlags` bins count the low four configuration bits (Enabled,
Periodic, Lazy, AutoEnable). Service/deadline-query visit counts measure host
scans, while delivery retries identify a busy or unavailable interrupt backend.
Counters are cumulative, diagnostic only, and do not suppress guest writes.

A repeated value is not sufficient evidence that an operation can be skipped.
Periodic counts are durations and must retain their rearm behavior; AutoEnable
can change the enabled state when a nonzero count is written. Zero counts stop
the timer. One-shot counts are absolute expiration times. The implementation's
128-bit timeline preserves periodic deadlines across reference-counter wrap.

These rules are checked against [Microsoft's synthetic timer specification](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers).
The existing native Hyper-V tests cover all timers/VPs, lazy deadlines, missed
periods, pending-message delivery retries, invalid registers and clock wrap.
The diagnostic regression also demonstrates that repeating a periodic count
restarts its period rather than silently discarding the write.

## Observed Windows behavior and implemented optimization

The native two-vCPU Windows run classified roughly 640–1,086 configuration
writes and 797–1,302 count writes per second across three 20-second windows.
Configuration writes used low-bit flag combinations 8 (AutoEnable) and 10
(Periodic + AutoEnable), both with Enabled cleared. The count write then
re-enabled the timer. Repeated count values occurred 131–314 times/sec, but
none of those windows contained a write leaving an already-armed deadline
unchanged. Periodic rearming was frequent. This is a disable/configure/rearm
sequence, not evidence of disposable duplicate writes.

The host previously walked all eight timer slots in every `service` call and
again in every deadline query. The new implementation caches the earliest
all-timer and non-lazy deadlines. It invalidates those derived values when
armed state, deadline or lazy membership changes, and after expiration/retry
processing. It still observes clock wrap and performs every guest write and
rearm. Before the earliest deadline, service returns without walking the
slots. Expired timers with a busy backend retain their pending deadline.

`serviceDelay100ns` also takes one consistent clock snapshot for its timer,
heartbeat, time-sync, shutdown and KVP deadline calculations. The regression
test checks that all enabled sources share that sample. It does not change
service intervals or remove guest timer programming, interrupts or barriers.

The optimized Windows run's three windows returned early on **97.27–98.16%**
of timer-service calls. Actual slot visits (cache rebuilds plus due-service
walks) were **56.21–61.55% lower** than the old eight-slot-per-call/query policy
would require for those same observed operation counts. This is a normalized
host-work comparison; different boots and Windows background tasks make the
raw total process CPU values unsuitable for attributing a Windows-wide saving.
Both boots reached the signed-in desktop. The optimized guest accepted keyboard
and absolute mouse input, displayed its Start menu, retained successful
heartbeat/time-sync exchanges without timeouts, and powered off through the
Hyper-V guest shutdown service.

## Isolated benchmark

```sh
./tools/build.sh hyperv-timer-bench
./build/hyperv-timer-bench 2000000 3
```

This is a RAM-only host benchmark: each loop services timers and queries both
all-timer and non-lazy deadlines. `quiet` writes a future count every fourth
loop, `writes` does so every loop, `disable-autoenable` alternates disabled
one-shot/periodic configurations followed by an AutoEnable count, and `periodic`
expires every 32 ticks. `eight-vp` exercises the larger configured VP count.
These are deterministic scheduling scenarios, not a replay of every Windows
hypercall. Nine trials per case were collected in alternating original/optimized
batches, compiled with the same Apple clang `-O1` settings. The original code
predates the diagnostic counters; the optimized code includes them.

| Workload | Original median CPU seconds | Optimized | Reduction |
| --- | ---: | ---: | ---: |
| Occasional writes, two VPs | 0.053857 | 0.023043 | 57.21% |
| Write each loop, two VPs | 0.060858 | 0.045335 | 25.51% |
| Disable/AutoEnable, two VPs | 0.056576 | 0.026368 | 53.39% |
| Periodic, two VPs | 0.052529 | 0.019761 | 62.38% |
| Eight VPs | 0.140526 | 0.018332 | 86.95% |

Deadline checksums and delivery counts were identical in every corresponding
trial. These percentages describe the isolated timer loop, not total Windows
idle CPU, VM-exit count, or the cost of guest execution.

Validation passed 2,704 Hyper-V model checks normally and under ASan/UBSan,
including cache invalidation after cancellation, earlier deadlines on another
VP, lazy/non-lazy changes, retries, periodic phases and 64-bit wrap. Native HVF
self-tests passed 304 checks; all 13 CLI integration tests passed after
regenerating the missing default platform assets from the earlier build cleanup.
The [measurement record](windows11-timers-2026-10-26.json) retains the original
and optimized trials, native counter endpoints and validation identities.
