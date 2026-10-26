# VirtIO MSI-X on ARM64

The modern PCI transport optionally exposes an MSI-X capability with one config
vector plus one vector per queue. It uses a 32 KiB BAR (INTx-only endpoints retain
16 KiB). The table and pending bit array are in BAR0 at 0x4000 and 0x5000.
Function/vector masks and bus-master disable retain pending messages; unmasking
replays them. Invalid vector assignments read back as NO_VECTOR. Device reset
clears queue/config vector assignments and pending state. MSI-X suppresses INTx;
disabling MSI-X retains the legacy path.

Network RX and TX completions identify their queue, so they reach the assigned
vector rather than broadcasting interrupts. The backend ISR latch is acknowledged
after a message is recorded (including when masked), allowing repeated completions
to generate independent edges. Counters report actual sent messages.

The HVF GIC provides a v2m MSI frame at 0x08020000 with SPIs 64–127. PCI messages
must target its SET_SPI_NSR register at 0x08020040. The owner validates address
and interrupt range before calling hv_gic_send_msi. ACPI MADT type 13 and the
device tree describe the same frame and range. Existing wired SPIs remain below
64. Hyper-V's nonsecure distributor view continues to use the native GIC backend.

Build matching platform assets with `tools/build_arm_platform.py --msi` and select
`pcemu-hvf --virtio-msix`. Other platform options (CPUs, RAM, Hyper-V and VMBus)
must match as before. MSI-X rejects assets without the expected MADT frame.
`run11.sh` now builds/selects matching MSI assets by default.

Validation on 2026-10-26:

- 599 VirtIO block/PCI checks, including queue/config routing, masks, PBA replay,
  bus-master gating, invalid assignments, reset and INTx fallback.
- 38 network and 46 RNG checks; RX/TX queue identity is tested separately.
- 304 native HVF checks, including real MSI injection, guest IAR acknowledgment,
  EOI and no duplicate delivery; invalid MSI targets are rejected.
- Seven platform-generation tests, four ACPI tests and 13 CLI integration tests.
- Windows 11 ARM64 booted and signed in. The guest selected MSI-X for block,
  network and RNG. DNS, HTTP download/upload and two explicit VirtIO CNG entropy
  reads (128 bytes) passed. The host observed 61,707 block, 1,666 network and two
  RNG MSI messages at capture. These are cumulative activity counters, not a
  throughput or latency benchmark. The existing viostor 0.1.285 boot driver was
  retained; newer driver compatibility is a separate check.

This is a shared transport prerequisite for additional devices, not completion
of GPU, filesystem, balloon, serial or synthetic Hyper-V network/storage support.

References: [VirtIO 1.3 PCI transport](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html),
[ACPI GIC MSI frame](https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model.html),
and the installed Apple Hypervisor.framework GIC API headers.
