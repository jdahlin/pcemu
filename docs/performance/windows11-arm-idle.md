# Windows 11 ARM idle profiling

2026-10-26, Apple Silicon macOS, 4 GiB guest RAM, two vCPUs, SDL software
renderer, user-mode virtio networking, existing qcow2 base/PCD overlay.
CPU percentages count 100% as one host logical CPU.

The settled baseline consumed 39.87%, 39.90%, and 43.61% CPU over three
20-second windows (median 39.90%). Network traffic was zero; disk activity
was 1.5–6.8 requests/sec. Host interrupt wakeups were 288–293/sec.
A five-second `sample` capture found guest execution and SDL presentation
prominent; network service appeared infrequently. Wall-stack sample shares
are not CPU-time percentages.

ARM user networking now waits for socket readiness on a one-shot poll worker.
The worker only requests a latched HVF exit; packet processing and guest memory
access remain on the owner thread. Flow lifetimes keep polled descriptors alive.
Actual retransmit/expiration deadlines replace the unconditional 2 ms run cap.
The x86 network path retains its existing polling behavior.

Validation: legacy and readiness-driven network suites cover TCP/UDP,
retransmission, forwarding, backpressure and reset. Native HVF self-tests cover
requests before and during a run. Windows downloaded a PowerShell probe over
HTTP, resolved example.com using DNS, and POSTed its report to the host; guest
keyboard input launched the probe. Artifacts and local scripts are ignored in
build/windows11-idle-profile/.

Initial network-only windows measured 36.43%, 36.19%, 19.13% CPU and
57–68 interrupt wakeups/sec. Disk/network activity varied, so these windows
are not an equivalent idle comparison or proof of a particular CPU saving.

The ARM display path now retains visible raw rows and compares them exactly
before pixel conversion and SDL presentation. Stride padding is excluded; a
changed descriptor forces refresh. Inactive/invalid modes invalidate the cache.
Input polling remains at the existing 16 ms cadence, and SDL expose, resize and
restore events request presentation even when guest pixels are unchanged.
The visible-row cache uses width × height × 4 bytes (1.83 MiB at 800×600).

Validation: 55 MMIO/PCI/framebuffer checks, 58 SDL frontend checks, shared input
policy tests and USB xHCI enumeration/HID/IRQ/reset/DMA tests passed. The network
suite passed 103 legacy checks and 105 readiness checks; the latter also passed
ASan/UBSan. Native HVF self-tests passed 287 checks. In the combined Windows build,
USB keyboard input signed in and launched the probe; mouse movement/right-click
opened the desktop context menu. DNS and HTTP download/upload passed again.

To reproduce, leave the SDL Windows desktop visible with no console or menus,
then run (PID is the actual bundled pcemu-hvf process):

```sh
python3 tools/profile_hvf_idle.py PID --log build/windows11-sdl/run.log \
  --output build/idle.json --settle 15 --duration 20 --count 3
sample PID 5 -file build/idle.sample.txt
```

Use the log path from the active launcher. The profiler requests SIGUSR1 live
counters before/after each interval, outside the CPU timer. It reads macOS
proc_pid_rusage and converts CPU-time mach ticks using mach_timebase_info
(125/3 ns per tick on this host), checked against process CPU time. Treat windows
with substantial guest disk/network activity as background work, not idle.
Do not run builds or active input tests during timed intervals. Sampling stacks
identifies call paths but includes blocked time.

## Measured result

| Run | Three 20-second CPU samples | Median | Interrupt wakeups/sec |
| --- | --- | --- | --- |
| Baseline, settled desktop | 39.87%, 39.90%, 43.61% | 39.90% | 288–293 |
| Network + unchanged-frame skipping, settled desktop | 5.75%, 7.28%, 6.79% | 6.79% | 59–61 |

The observed combined median is 83.0% lower. These are separate boots,
not a deterministic guest workload: baseline disk activity was 1.5–6.8 requests/s,
combined 3.2–9.9 requests/s; baseline networking was zero, combined 0–2.75 packets/s.
Windows background tasks introduce variation. The network-only samples were
inconsistent (including a disk-heavy 96% CPU interval), so they do not isolate
network versus display CPU savings.

The post-change five-second stack sample contained no SDL present or pixel
conversion calls; visible-row comparisons remained, and the socket worker was
blocked in poll. Before, SDL present appeared in 609 main-thread wall samples
and pixel conversion in 72. This confirms removal of repeated work; those stack
counts are not CPU percentages. Remaining sampled paths include guest execution,
HVF waits and vCPU synchronization; no further optimization is claimed here.

Raw intervals, including background-active runs, binary identity and configuration
are retained in [the measurement record](windows11-arm-idle-2026-10-26.json).

## Residual CPU follow-up

A subsequent boot used the same executable code/data segments (the copied app
was re-signed), 4 GiB/two vCPUs, with the desktop visible. Six 30-second windows
after 15 seconds settling measured **34.05%, 23.42%, 18.92%, 8.68%, 12.57%,
19.67% CPU** (median 19.29%). Disk requests/sec were respectively 131.18, 34.33,
2.47, 4.97, 5.97, 17.23. This does **not** reproduce a stable 7% baseline:
Windows work changes between boots and continues with little disk traffic.
The earlier 6.79% observation remains valid for its recorded windows, but is
not a guaranteed idle level.

The profiler now supports `--threads`, using `PROC_PIDLISTTHREADS` plus
`PROC_PIDTHREADINFO`. These thread counters are already nanoseconds; process
rusage times still require mach timebase conversion. The CPU sums matched the
process within 0.108 percentage points over all six windows. Transient threads
missing at an endpoint are excluded, and snapshots are sequential.

Thread roles were identified from stack samples and matched to thread handles
using cumulative counters from `PROC_PIDTHREADID64INFO`: the four matches differed
by only 0–42 microseconds. This avoids guessing roles from thread enumeration
order. Instruments/xctrace was unavailable on this host.

| Role | Quietest window (8.68% total) | Range over all six windows |
| --- | --- | --- |
| Main / vCPU 0 / device processing / display | 4.98% | 4.98–20.48% |
| Secondary vCPU 1 | 2.60% | 2.60–10.44% |
| Run-deadline watchdog | 1.01% | 1.01–2.76% |
| Network readiness worker | 0.0005% | 0–0.021% |

The five-second quieter stack capture showed primary and secondary vCPUs mostly
waiting inside HVF, with secondary finish/dispatch synchronization also present.
The watchdog waited/rearmed its condition-variable deadline; the network worker
blocked in poll. There were no SDL present or pixel conversion frames, and only
12 main-thread wall samples in visible-row comparison. Waiting stack counts are
not charged as CPU time; the table above comes from actual CPU counters. The
stack capture overlapped the fourth interval, so its observer overhead is not
isolated.

Hyper-V counter snapshots around these windows recorded approximately 1,982–3,798
timer configuration/count writes per second (`0xb0000`/`0xb0001`), 42–69 synthetic
timer deliveries/sec, 307–547 IPI calls/sec and only 0.7–6.7 context-sync calls/sec.
Those counts identify frequent exits, not their individual CPU costs. In
particular, this does not justify blaming instruction-context barriers for the
remaining load.

The next measured candidate is the run-deadline watchdog: `HvfMachine::run`
increments generation, arms and notifies it for every run, then disarms/notifies
it after the exit. Thousands of short exits therefore repeatedly rebuild a timed
wait, and the thread alone consumes 1–2.76 CPU percentage points. A follow-up
should test retaining the deadline across short exits or reducing notifications,
while preserving cancellation, input latency and timer deadlines. The remaining
main/secondary CPU includes guest execution and host coordination; these external
profiles cannot precisely separate their CPU costs. Per-phase CPU instrumentation
would be needed before claiming savings there. No additional execution behavior
was changed in this follow-up.

Raw windows, thread matches, binary identities and Hyper-V counter rates are in
[the residual measurement record](windows11-arm-residual-2026-10-26.json).
Local stack captures remain in `build/windows11-idle-profile/residual-*.sample.txt`.
Reproduce the CPU split by adding `--threads` to the profiler command above.
