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
