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

Next, register MMIO regions and PCI endpoints outside the vCPU exit loop, and
extract the RAM framebuffer adapter from the machine. Keep CPU syndrome decoding,
register extension and PC advancement in the execution boundary. Keep device
register behavior in devices and address/IRQ wiring in platform composition.
