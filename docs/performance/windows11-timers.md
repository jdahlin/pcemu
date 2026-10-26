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
