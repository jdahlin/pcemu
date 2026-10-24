# ARM64 Hypervisor.framework backend

`make test-hvf` builds and runs an optional native Apple Silicon backend.
It runs flat ARM64 code in a hardware vCPU, independently of the existing x86
CPU, JIT, PC devices and SDL frontend. It does not yet boot Linux or Windows.
Windows 3.11/95 remain on the legacy x86 platform.

```
make build/pcemu-hvf
build/pcemu-hvf --list-machines
build/pcemu-hvf --machine arm64-test --backend hvf --self-test
build/pcemu-hvf --kernel guest.bin --timeout-ms 1000
```

The target links only Hypervisor.framework and C++ system libraries. It requires
an Apple Silicon macOS host, checks `kern.hv_support`, and ad-hoc signs the binary
with `com.apple.security.hypervisor` from
`platform/macos/hypervisor.entitlements`. No root privileges or host configuration
changes are needed. This signing step applies only to the experimental runner.
The portable x86 targets do not link Hypervisor.framework.

## Current platform contract

- Profile `arm64-test`, platform `arm-baremetal-v1`, backend `auto` or `hvf`.
- One VM per process, one vCPU, 2 MiB anonymous RAM mapped at guest physical
  address `0x40000000`. The backend class accepts page-aligned RAM up to 256 MiB;
  the initial runner uses 2 MiB. There are no disk images or firmware downloads.
- Raw, little-endian ARM64 instructions loaded at RAM base. Initial mode EL1h,
  MMU and caches disabled, DAIF interrupts masked, stack at RAM end. Other
  registers retain their vCPU values; this is not a Linux/UEFI boot protocol.
- RAM is directly hardware mapped with guest read/write/execute access. Host
  mapping is read/write; loading code invalidates the host instruction cache.
  Arithmetic and RAM accesses run without per-instruction callbacks or exits.
- `HVC #0` is the runner's completion convention. It prints X0 and exit details.
  Other exceptions and cancellations return a failure exit status. No general
  hypercall or device ABI is defined yet.
- A persistent watchdog thread requests `hv_vcpus_exit` at the run deadline
  (1–60000 ms). The deadline is a cancellation request, not a real-time guarantee
  or an instruction budget. It adds no periodic guest interrupts/exits.
- The backend exposes exception syndrome, PC and fault address so a platform can
  implement MMIO and explicitly retire the trapped instruction. It does not
  silently skip unexpected traps. Virtual timer exits are reported; timer/IRQ
  injection and a guest interrupt controller are not implemented.

Create/use/destroy `HvfMachine` on its owning thread. The watchdog uses only the
cross-thread cancellation API. Public register/memory/run calls reject the wrong
thread; destruction on the wrong thread terminates rather than violating the
Hypervisor API. Do not put a vCPU on a migrating dispatch queue. Cleanup stops
and joins the watchdog, destroys the vCPU, unmaps/frees RAM and destroys the VM.
Partially failed initialization releases resources it acquired. A cancellation
racing a completed exception can leave a pending canceled exit on the next run;
callers may resume it, subject to their overall task deadline.

## Verification

`make test-hvf` runs 43 backend checks and four CLI integration tests:

- Twice create/destroy a VM; reject a second concurrent VM in the same process.
- Execute a 10-million-iteration add/subtract/branch loop plus RAM store/load and
  HVC: **30,000,003 guest instructions, one VM-exit**, with exact register/RAM
  results checked. Report elapsed wall time; this is a small compute test, not an
  OS performance result or a Rosetta comparison.
- Trap a 32-bit MMIO write/read, validate syndrome/address/register, supply the
  emulated read response and resume to HVC.
- Cancel an infinite guest loop and resume the same vCPU with new code.
- Reject invalid RAM/ranges/registers/entry alignment/timeouts and thread use.
- Load a separate tiny raw guest returning 42; verify CLI timeout, invalid input,
  incompatible machine/backend choices and help/catalog output.

The minimal Windows ARM64 payload is documented in
[Windows ARM64 test media](windows-arm64-media.md). It is downloaded separately
and cannot be booted by this raw-code runner yet.

## Next platform work

Choose and version the ARM guest hardware contract before adding an OS image:
physical memory map, firmware load/entry, boot parameters or UEFI, interrupt
controller, ARM generic timer, UART and storage. A Linux serial boot is a useful
next milestone before SMP and richer devices. Keep normal RAM and CPU work in
the guest, handle device exits in batches where possible, and measure exit counts
by reason before optimizing.

Windows 11 ARM64 additionally needs a compatible UEFI/ACPI platform, supported
interrupt/timer/SMP behavior, TPM2/Secure Boot and Windows-compatible device
interfaces and ARM64 drivers. Graphics acceleration needs its own guest/host
integration. It remains a planned profile; the existence of a vCPU backend does
not make it bootable. Apple hardware virtualization does not execute x86 code on
ARM; Windows 11 x64 is a separate emulation project.

Do not serialize HVF handles into legacy snapshots. Add versioned ARM CPU/device
state and platform/firmware identity when checkpoint support is implemented, then
share the existing compressed COW disk storage where suitable. This backend
currently has no snapshots, guest disks, networking or frontend.

References consulted:
- [Apple Hypervisor framework and lifecycle](https://developer.apple.com/documentation/hypervisor)
- [Apple vCPU management and thread affinity](https://developer.apple.com/documentation/hypervisor/vcpu-management)
- Local macOS SDK: `Hypervisor/hv_vm.h`, `hv_vcpu.h`, `hv_vcpu_types.h`.
