# Debian ARM64 platform audit

Audit date: 2026-10-24. Guest: Debian 13.7, kernel
`6.12.107+deb13-arm64`, native HVF ARM64 UEFI profile, one vCPU and 2 GiB RAM.
This is an implementation roadmap based on a running guest, not a claim that
these features have been implemented.

## Evidence

The ignored `build/debian-platform-audit/` directory contains `guest-audit.log`,
`dmesg-audit.log`, `kernel-journal-audit.log` and `warnings-audit.log`.
Kernel logs were captured through a temporary EFI diagnostic boot using
`systemd.debug_shell=ttyAMA0`; normal boot was restored afterwards. No persistent
sudo or kernel-log access permissions were changed.

The server boots, SSH works, no systemd services failed, and virtio network
counters showed no errors or drops. Linux uses `arch_sys_counter` at 24 MHz.
The following priorities address observed gaps and useful host integration.

## Implemented after the audit

The host-backed PL031 RTC now provides EFI time services. Debian registers
`rtc-efi` and boots with the correct date (2026-10-24), verified over SSH.
NTP/precision time synchronization remains separate work. Virtio-rng now supplies
firmware and Linux entropy: a real boot reports `KASLR enabled`, CRNG ready at
time zero, and `virtio_rng.0` selected. The original findings below are retained
as the audit baseline.

## Timer capability clarification

The initial GSI0 warning was classified too strongly as a firmware defect.
Apple's HVF SDK explicitly provides the EL2 timer interrupt only when nested
virtualization is enabled. This profile disables EL2, so GTDT leaves the EL2
fields zero and the firmware device tree now omits the EL2 timer interrupt.
Linux 6.12 nevertheless attempts to map the zero entry and prints the warning.
It remains visible; assigning an unavailable PPI merely to hide it would be
incorrect. Working EL1 timer delivery and clocksource selection were verified.

The installed guest deliberately uses the RTC without an additional NTP service.
Guest-requested reboot now recreates the platform in the same runner; Debian
SSH reconnect, a changed boot ID and file persistence were verified. HVF now
advances the virtual counter for host sleep time, with injected sleep/timer tests
and a live Debian process-pause check. Physical Mac sleep/wake and precise PTP
integration still require verification.

## Recommended order

| Priority | Work | Evidence and acceptance criteria |
| --- | --- | --- |
| P0 | RTC and wall-clock synchronization | Guest UTC was April 13 while host date was October 24. No RTC and no active NTP service; systemd falls back to its build epoch and journals report backward jumps. Implement a host-backed RTC (PL031 fits the existing firmware library), expose it in firmware/platform descriptions, and enable guest time synchronization. Verify cold boot time, host sleep/resume, monotonic continuity and TLS/package operations. |
| P2 | Review GTDT/Linux absent-EL2 compatibility | `Illegal GSI0 translation request` comes from Linux mapping the absent EL2 timer. HVF provides that interrupt only with nested virtualization. Keep EL2 fields zero and omit its device-tree interrupt; verify working EL1 timers. A warning-free solution needs a compatible guest-kernel/platform approach, not an invented PPI. |
| P1 | Host entropy through virtio-rng | EFI reports `EFI_RNG_PROTOCOL unavailable`; Linux reports `KASLR disabled due to lack of seed`. Firmware already includes VirtioRngDxe and Linux supports virtio RNG as a module. Verify firmware discovers it, KASLR receives an early seed, and Linux selects the device. `random: crng init done` does occur, so the running guest is not without a functioning CRNG. |
| P1 | SMP, PSCI and CPU/cache topology | Only one vCPU runs; `No PPTT table found` and `Unable to detect cache hierarchy`. Implement secondary-vCPU lifecycle, correct PSCI discovery/calls and matching MADT/PPTT descriptions. Verify concurrent load, CPU start/stop and idle. Do not invent physical host cache topology for a virtual CPU. |
| P1 | Guest integration channel and lifecycle | No vsock or virtio ports. Add virtio-vsock or virtio-serial with a narrowly scoped guest agent for readiness, heartbeat, clean shutdown/reboot and filesystem freeze/thaw. Reboot currently exits the runner; support an actual restart lifecycle. Verify disconnect/reconnect, agent failure and clean disk recovery. |
| P2 | Storage and network efficiency | Block device reports rotational media and no discard support. Implement honest device properties and safe DISCARD/WRITE_ZEROES for the COW backend. Current virtio uses split queues and legacy INTx; evaluate MSI-X, indirect descriptors, multiqueue and network segmentation offload after SMP. Benchmark CPU cost, throughput, latency and idle wakeups before selecting optimizations. |
| P2 | Memory, file sharing and profiling | Ballooning requires real host page reclamation, not just guest accounting. Virtio-fs is useful when shared folders are needed. `No ACPI PMU IRQ for CPU0` means profiling support needs investigation; a working cycle counter does not establish full interrupt-driven PMU support. |

The host network loop currently caps runs at 2 ms and the gateway polls at 2 ms.
An event-driven wakeup path is another concrete opportunity to reduce idle
CPU usage; measure it alongside interrupt/queue changes.

## Hyper-V-like extensions

For Debian, use ARM architectural interfaces and virtio to provide the same
user-visible benefits. Hyper-V on ARM64 is a real hypercall/VMBus contract;
advertising a feature flag alone would not provide integration services.
A full Hyper-V implementation belongs in a separate Windows compatibility track.

* **Time:** the architectural counter already supplies the fast monotonic clock.
  RTC/NTP fixes wall time first. A KVM-compatible ARM PTP interface can provide
  precise host wall-clock/counter cross-sampling; this guest enables
  `CONFIG_PTP_1588_CLOCK_KVM=y`. Wall time and stolen-time accounting solve
  different problems.
* **Stolen time:** ARM PV time uses SMCCC discovery and a shared accounting
  structure. This Debian kernel has `CONFIG_PARAVIRT=y`, but
  `CONFIG_PARAVIRT_TIME_ACCOUNTING` is disabled, so implementing the host side
  alone would not benefit this kernel. Report only involuntary descheduling.
  HVF cumulative execution time is insufficient by itself: wall time minus
  execution time also includes intentional waits and device emulation.
* **Integration services:** virtio-vsock/serial plus a guest agent can provide
  heartbeat, shutdown and coordinated snapshots. Add explicit protocol versions,
  bounds and lifecycle tests before trusting agent requests.
* **Memory and I/O:** virtio balloon, RNG and optimized queues cover useful
  equivalents without requiring Linux to use VMBus devices.

`kvm: HYP mode not available` is expected for an EL1 guest without nested
virtualization; it does not mean HVF acceleration is missing. Missing TPM,
DMI and a hibernation image are not blockers for this minimal server. PCI
resource-reservation warnings merit a separate ACPI namespace review.

## Primary references

* [ARM PV time ABI](https://docs.kernel.org/next/virt/kvm/arm/pvtime.html)
* [ARM KVM PTP interface](https://docs.kernel.org/next/virt/kvm/arm/ptp_kvm.html)
* [Virtio 1.3 specification](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)
* [Hyper-V ARM64 hypercall interface](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface)
* [Linux VMBus documentation](https://docs.kernel.org/virt/hyperv/vmbus.html)
* [Linux GTDT implementation](https://android.googlesource.com/kernel/common/+/ed9be64eefe2/drivers/acpi/arm64/gtdt.c)
