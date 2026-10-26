# Windows 11 ARM64: Instruments idle investigation

Measured 2026-10-26 on an Apple M1 Pro, 10 logical CPUs, 32 GiB RAM; Windows
11 ARM64 build 26300.9457, 4 GiB, two vCPUs, VirtIO GPU/serial/balloon,
VMBus, MSI-X and user networking. CPU percentages use **100% = one host logical
CPU**. This is the development `-O1` executable, not an `-O3` release comparison.
No emulator execution behavior was changed in this investigation.

The main finding is that **no input does not imply a quiescent guest**. The
signed-in desktop and the Edge first-run welcome screen produce quite different
work. The largest sampled path is HVF guest execution/entry/exit. CPU-to-CPU
coordination is measurable. When the foreground UI keeps updating, the current
full-frame GPU conversion and SDL presentation become another useful target.

## Measurements and scope

The first run used SPICE with a real client connected and the signed-in desktop
visible. Three uninstrumented 20-second windows measured 47.09%, 17.22%, 32.55%
CPU, with 60–119 disk requests/sec: background-active, not a stable idle baseline.
Its subsequent 30-second Time Profiler window measured 14.29% process CPU,
4,562 VM exits/sec and 27.37 disk requests/sec. The SPICE worker consumed only
0.0022% CPU; GPU commands and serial bytes did not increase during that window.

A concurrent cleanup of `build/` removed those raw captures after their export.
Their already-observed results are retained as **session notes**, not as
recoverable raw traces. The primary retained recording below was therefore
saved outside `build/` and collected from the subsequently running SDL session.
Its foreground was the static-looking Edge first-run welcome screen. This is
not an equivalent replacement for the empty desktop, and the differences are
not an A/B estimate of SDL versus SPICE overhead.

| Retained SDL phase | Duration | Process CPU | Interrupt wakeups/sec |
| --- | ---: | ---: | ---: |
| No deliberate input, control 1 | 20 s | 85.57% | 340 |
| No deliberate input, control 2 | 20 s | 55.56% | 260 |
| Counter interval around 30 s Time Profiler recording | 32 s | 64.95% | 285 |

The guest continued producing roughly 14–21 GPU transfers/sec and 14–20
flushes/sec. In the instrumented counter interval there were approximately
3,651 legacy Hyper-V calls/sec, 966 IPI requests/sec, 247 synthetic timer
deliveries/sec, and 210 context-sync calls/sec. Timer-count/configuration
writes were about 1,365/1,106 per second. These counters overlap; **do not add
them together as independent exits**. Rates use host snapshot timestamps,
rather than assuming the reported guest reference clock is fresh.

## What Instruments shows

The retained Time Profiler recording resolves the actual C++ functions and
source locations. The following are shares of sampled running CPU weight,
not percentages of total machine capacity or time spent blocked.

| Path | Sample share | Interpretation |
| --- | ---: | --- |
| `hv_trap` leaf | 72.30% | Guest execution and hypervisor work; cannot identify a Windows process from this host stack |
| `VirtioGpu::changedPixels` inclusive | 5.01% | Full scanout conversion following changed-generation notifications |
| SDL `present` inclusive | 4.94% | Texture upload/render/presentation path |
| Condition-variable wait/broadcast/signal leaves | 4.40% | CPU spent entering/leaving synchronization, not blocked wall time |
| `HvfSecondary::dispatch` inclusive | 0.92% | Dispatching jobs to the secondary vCPU owner thread |
| `runMaintenance` inclusive | 0.15% | Context maintenance is frequent but not a leading direct CPU cost here |
| `VirtioGpu::pointerShape` inclusive | 0.03% | Cursor snapshot construction is a low-priority micro-optimization |

The main thread and secondary vCPU account for about 59.5% and 36.1% of sampled
weight. Actual process/thread counters during the surrounding interval measured
38.59% CPU for the main thread and 23.52% for the secondary vCPU. The watchdog
was approximately 0.38% CPU. The platform event thread was 0.93% CPU.

The earlier, less display-active SPICE capture put 61.89% of samples in
`hv_trap`, with 17.13% in condition-variable leaves. It had **zero GPU updates**.
That supports looking at vCPU coordination independently of display work, but
it is not a promise that a fixed fraction of CPU can be eliminated.

`hv_trap` is not synonymous with emulator overhead. It includes time charged
while the guest runs and hypervisor entry/exit work. Windows kernel/process
profiling or additional per-exit phase counters are needed to divide it further.
A separate guest process probe was also affected by its visible terminal;
minimizing the terminal changed that workload. It is not used to attribute
these retained host samples to specific Windows applications.

## Improvements worth testing

1. **Track GPU damage and skip unchanged content before conversion.**
   `RESOURCE_FLUSH` currently increments the display generation for the scanout
   resource. `changedPixels` then converts the entire visible image, and SDL
   uploads the entire texture. It does not retain the guest's dirty rectangle
   through the frontend boundary. Intersect damage with the scanout, compare
   affected raw rows when useful, convert only changed regions, and support
   partial texture/SPICE updates. Keep full refreshes for mode switches, expose
   events and reconnects. This directly targets the roughly 10% combined sampled
   conversion/presentation paths in the retained UI-active recording. Actual
   savings require a controlled replay; there is no measured improvement yet.

2. **Reduce secondary-vCPU handshakes and exit bookkeeping.**
   Every run dispatches the secondary vCPU; a real exit cancels its peers and
   waits for them before device emulation. Secondary jobs use a mutex,
   `std::function`, condition-variable notification and completion handshake.
   Non-cancelled secondary exits also fetch all 31 general registers. Measure
   lazy register reads, a dedicated run-job representation, and notifications
   targeted to the actual waiter. Any batching/fast path must preserve the
   existing rule that guest CPUs are stopped before emulated DMA touches RAM,
   and preserve cancellation, shutdown and input latency. Do not simply let a
   vCPU continue while device code accesses guest memory.

3. **Investigate why Windows reprograms timers so often, then optimize proven
   hot emulation paths.** There are thousands of timer-register writes per
   second, plus substantial IPIs. Add an exit-reason/register histogram and
   per-phase CPU timing to separate necessary guest work from dispatch cost.
   Cache equivalent work only where protocol semantics allow it. Do not remove
   barriers, suppress IPIs, or coalesce timer updates on the assumption that
   counts alone establish redundancy: `runMaintenance` is only 0.15% directly
   sampled in this retained capture.

4. **Keep smaller polling/cursor changes in proportion.** Frontend input is
   checked on a 16 ms cadence, and the GPU cursor image is reconstructed before
   the frontend compares it. A cursor-specific revision and event/deadline-based
   input service could remove needless work. But cursor construction is only
   0.03% of this sample, and the quiet SPICE worker was negligible. These should
   follow the larger coordination/damage-path candidates.

Separately, the initial guest report labelled its UTC time about seven hours
ahead of the host receipt time. That warrants an RTC-local/UTC and Hyper-V
clock synchronization audit. Concurrent RTC work was in progress; this report
does not diagnose the cause or claim it explains CPU usage. All CPU intervals
here use host monotonic time.

## Measurement cautions and reproduction

A System Trace experiment produced a strongly perturbed interval around 180%
process CPU. It is not used as an idle baseline or compared as a regression.
Use Time Profiler plus independent process CPU counters first. Avoid building,
typing, visible diagnostic terminals, and background cleanup during a capture.
This image also continues background disk/network activity after logon, so a
single low or high interval should not be reported as its permanent idle cost.

For a stable PID and active log path:

```sh
python3 tools/profile_hvf_idle.py PID --log ACTIVE_LOG \
  --output /outside/build/cpu.json --settle 30 --duration 20 --count 3 --threads
xcrun xctrace record --template 'Time Profiler' --attach PID \
  --time-limit 30s --output /outside/build/idle.trace --no-prompt
xcrun xctrace export --input /outside/build/idle.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' \
  --output /outside/build/time-profile.xml
```

The retained `.trace` is at
`/Users/johandahlin/dev/pcemu-profile-artifacts/windows11-idle-2026-10-26/retained-time.trace`.
The capture/analysis scripts, screen and uncompressed export are beside it.
The [measurement record](windows11-instruments-2026-10-26.json) includes raw
process/thread intervals and device-counter endpoints, executable identity,
full sampled-function tables and explicit scope notes. The exact exported
[samples are retained compressed](../measurements/windows11-instruments-2026-10-26.xml.zst).
