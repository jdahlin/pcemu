# Debian ARM64 device composition

Debian uses the existing `ArmVirtMachine` / `arm-virt-v3` platform. Device
refactoring preserves its firmware contract, addresses, interrupts and CLI.
It does not alter the running installed VM or its private disks/variables.

## Device ownership

| Component | Files | Responsibility |
|---|---|---|
| Serial console | `arm_pl011.*` | PL011 registers, FIFO, transcript and IRQ level |
| Real-time clock | `arm_pl031.*` | PL031 registers, host clock adjustment and alarms |
| Firmware variables | `arm_nor_flash.*` | NOR command state, erase/program and dirty ranges |
| Power protocol | `arm_psci.*` | Single-vCPU PSCI calls and requested power action |
| Debug OS lock | `arm_os_lock.*` | Trapped OS-lock register state |
| Firmware configuration | `arm_fw_cfg.*` | fw_cfg files and DMA |
| RAM framebuffer | `arm_ramfb.*` | Guest descriptor validation and pixel export |
| Block device | `virtio_blk.*` | Block commands and virtqueues |
| Network device | `virtio_net.*` | Packet queues and virtio network registers |
| Entropy device | `virtio_rng.*` | Entropy requests and virtqueue |
| Virtio PCI transport | `virtio_pci.*` | PCI configuration and virtio capabilities |
| NVMe controller | `nvme.*` | NVMe registers, queues and namespace |
| Host services | `disk.*`, `network.*` | Storage formats and host socket gateway |

`arm_mmio.*` handles AArch64 load extension rather than device state.
GIC and architectural timers are provided by Hypervisor.framework through
`hvf_machine.*`, not reimplemented as software peripheral models.

The first extraction removed the combined `arm_devices.*`; tests include each
device header explicitly. Existing portable device tests and `make test-hvf`
passed before and after this file separation. Local logs are
`build/debian-device-baseline.log` and `build/debian-device-split-tests.log`.

## Buses and platform composition

`arm_virt_devices.cpp` registers devices in `MmioSpace` (`mmio_space.*`) and
`PciEcamBus` (`pci_ecam.*`). The vCPU exit loop only decodes the transaction,
dispatches it, extends a load into the destination register and advances PC.
Device register behavior stays in device files; address/IRQ wiring stays in
platform composition. The machine still owns device lifetimes and host resources.

Fixed MMIO regions preserve the previous decoder's precedence and access widths.
Each device validates alignment and transfers crossing its register boundaries.
Read-only/write-only mappings can reject the other direction. The NOR write
handler synchronizes only dirty bytes back to its mapped ROM; array reads remain
directly mapped. PL031 discarded loads retain their old behavior, while UART
FIFO reads still consume data even when the CPU discards the value.

The PCI bus routes function-zero ECAM configuration to slots 0–3 (boot storage,
extra storage, network, entropy). Absent slots/functions read as all ones and
ignore writes. BAR matching queries live endpoint state after relocation or
memory-decoding changes. Later endpoint registration wins overlapping BARs;
NVMe is registered last to retain its previous priority. Device-specific virtio
errors remain checked after PCI accesses. Transport/device implementations are
shared between configurations, including the existing virtio-MMIO boot option.

Portable bus tests cover MMIO bounds, directions, widths, overlap rejection,
dynamic-route precedence, ECAM holes, BAR relocation/disable, slot collisions,
framebuffer bounds, stride and exact RGB output. Existing device tests continue
to validate the individual UART/RTC/NOR/PSCI/fw_cfg and virtio/NVMe models.

No guest-visible address, SPI number, firmware descriptor, CLI profile or storage
format changed. GIC/timer emulation remains delegated to the hardware backend.

## Validation of the composed platform

All runs used headless guests and fresh private test media/variables. The running
installed Debian instance was neither stopped nor used for these boot checks.

- `make test-hvf`: portable peripheral/virtio/NVMe/storage tests, backend
  self-test and CLI/ACPI checks passed. The bus/framebuffer suite has 32 checks.
- `test_debian_media.py` passed; `smoke_debian_arm64.py` reached installer
  language selection through real UEFI/Linux boot. This is not an installed
  server or authenticated SSH regression test.
- `smoke_arm_integration.py` verified EFI time, entropy and PSCI power-off.
- `smoke_arm_acpi.py` verified the firmware ACPI/GIC/timer/PMU contract.
- `smoke_virtio.py` booted via both PCI and MMIO, wrote a guest file and read it
  after VM recreation, and verified immutable source-disk hashes.
- `smoke_vhdx.py` passed independently with `virtio` and `nvme`: direct writes
  survived VM recreation, with no COW files substituted for direct VHDX access.

Local evidence is retained under `build/debian-device-*.log`: `router-tests`,
`bus-tests`, `media`, `boot`, `firmware`, `acpi-boot`, `transports`, `virtio` and
`nvme`. Each guest smoke log points to its generated serial/host logs and summary.
