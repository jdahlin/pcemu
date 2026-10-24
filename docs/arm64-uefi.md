# ARM64 UEFI platform

`arm64-uefi` boots real EDK II firmware to its built-in UEFI Interactive Shell
using Hypervisor.framework. Serial command execution and nonvolatile variables
survive a complete VM teardown and restart. The platform also [boots EFI applications from virtio disks](virtio.md).
Linux and Windows boot are not yet implemented.

Requires Apple Silicon, macOS 15 or newer (for GICv3), the command-line C++ tools,
and `dtc` on PATH. The existing SDL/x86 emulator remains independent.

```
make smoke-uefi
```

This builds/signs `pcemu-hvf`, compiles the platform device tree, downloads a
pinned firmware package if needed, and performs two real firmware boots. To run
just to the prompt:

```
build/pcemu-hvf --machine arm64-uefi --until-text 'Shell> '
```

To send a command once the shell is ready:

```
build/pcemu-hvf --machine arm64-uefi --shell-command ver --timeout-ms 15000
```

Serial output goes to stdout, including firmware ANSI terminal sequences.
`--shell-command` can contain carriage-return-separated commands. With
`--until-text`, a command's match is searched only after it has been sent.
Without an output criterion, the runner stops normally at its wall-clock deadline
(default 30 seconds). With a criterion, timeout returns failure. This is a bounded
serial runner, not yet an interactive terminal or graphical frontend.

## Firmware and disk space

`tools/fetch_arm64_uefi.py` fetches Debian's EDK II build
`qemu-efi-aarch64_2025.02-8+deb13u1_all.deb`, checks its pinned published SHA-256,
and extracts only:

- `roms/edk2-arm64.bin`: 3 MiB firmware.
- `roms/edk2-arm64-vars.bin`: first 768 KiB of the variable template. The remaining
  padding of Debian's 64 MiB variable image is not stored or advertised.
- Debian's readme/license and a checksum/source manifest in `build/arm64-fw/`.

Firmware binaries and downloaded packages are ignored by Git. The 4.9 MiB package
is cached; later calls verify existing assets and skip download/decompression.
No QEMU executable, CPU core or device implementation is linked or executed.
This is firmware reuse; device emulation is original code in this repository.

Each VM uses `--vars PRIVATE_FILE`, default `build/arm64-uefi/vars.bin`.
The file is 768 KiB and is created from the shared template on first successful
run. An adjacent advisory lock prevents concurrent access by these runners.
Saving writes a temporary file and atomically replaces the private store;
status/ID overlays are not persisted. Using the template itself as a writable
store is rejected. This is process-safe save/reload, not a VM snapshot or a
power-loss durability guarantee.

## Guest platform contract: arm-virt-v2

| Address | Device |
|---|---|
| `0x00000000` | Read/execute firmware ROM, reset entry at zero |
| `0x04000000` | 768 KiB NOR variable store, guest writes trapped |
| `0x08000000` | Hypervisor.framework GICv3 distributor |
| `0x080a0000` | GICv3 redistributor for the single vCPU |
| `0x09000000` | Polled PL011 serial console |
| `0x0a000000` | Optional virtio-blk MMIO device |
| `0x10000000`–`0x3effffff` | PCI memory window |
| `0x3f000000` | PCI ECAM, bus 0 |
| `0x40000000` | 256 MiB RAM; device tree initially at RAM base |

The CPU starts at EL1h with translation disabled and DAIF masked. EDK II initializes
its MMU, interrupt controller and architectural timer. The physical timer uses
PPI 30; GICv3/timer operation is managed by Hypervisor.framework. Firmware output
and a progressing shell countdown verify the current timer path. No EL3 monitor
or nested virtualization is used. PSCI is advertised for the platform contract,
but PSCI calls, SMP, guest reset/poweroff are not implemented yet and fail clearly
if trapped; only the single-CPU firmware path is tested.

`platform/arm64/pcemu-virt.dts` supplies the versioned hardware description.
The runner expects that compiled tree and its fixed RAM map; accepting a path to
an arbitrary DTB does not make arbitrary hardware layouts supported. Device tree
is sufficient for this firmware milestone; Windows ACPI/SMBIOS tables are still
missing.

`src/arm_virt.cpp` owns firmware loading, MMIO dispatch and variable persistence.
It uses the separate `HvfMachine` execution backend, not the x86 `Machine`.
`src/arm_devices.cpp` provides deliberately limited firmware device models:

- PL011 polled transmit/receive, selected control registers and PrimeCell IDs.
  Input is a bounded host queue. UART IRQ delivery and DMA are not implemented.
- NOR x32 access for two interleaved x16 parts, 256 KiB erase blocks, word and
  buffered programming, ready status and unlocked ID reads. It implements the
  sequences exercised by the pinned EDK II driver, not the complete Intel/CFI
  command set or lock/protection behavior. Unknown commands fail instead of
  silently reporting success. Normal array reads are directly mapped; command
  writes trap and update only dirty ranges, with read-status overlays restored
  before returning to array mode.

RAM and GIC register access do not pass through the userspace MMIO dispatcher.
The UART and flash generate exits; the runner currently processes these one
access at a time. Virtio-blk PCI/MMIO storage is now available; see [virtio](virtio.md).
The original fw_cfg MMIO device at `0x09020000` supplies a writable `etc/ramfb`
file. Its directory, stream reads and synchronous DMA follow the
[documented fw_cfg wire interface](https://www.qemu.org/docs/master/specs/fw_cfg.html).
No QEMU device code is linked. The existing EDK II Ramfb driver installs GOP and
allocates its framebuffer inside guest RAM; default mode is 800×600 XRGB8888.
`--framebuffer-ppm build/console.ppm` exports a bounded RGB image at the end of
an observation run. This is a diagnostic image, not a live display frontend.
DMA is limited to guest RAM; read-only and out-of-bounds file writes fail without
partially changing the file. An invalid descriptor address is a platform error.

There is no ACPI, TPM or Secure Boot implementation. Secure Boot is not enabled for the
shell milestone. Windows Validation OS requires additional platform work.

## Verification

`make test-hvf` includes backend ROM/GIC tests, 58 portable NOR/UART/MMIO-load/fw_cfg checks and 565 virtio block/PCI checks and
CLI tests for private template protection and variable-file locking.
`make smoke-uefi` creates a small private directory under `build/uefi-smoke-*`:

1. Boot EDK II, wait for `Shell>`, set `PcemuNv=PCEMU_NV_OK`, and execute `echo`.
   Require the actual CRLF-delimited response, not the injected command echo.
2. Save the private NOR store, destroy the VM, create a new VM using that file,
   and execute `echo %PcemuNv%`. Require the saved value as guest output.

The test retains two small serial logs, host logs, its 768 KiB variable file and
`summary.json`, and two 1.4 MB PPM console images. The test requires GOP to be
configured and the captured console image to contain nonuniform pixel data.
No guest disks or RAM dumps are created. The shared firmware
and variable template are not changed.

References:
- [Debian firmware package and published checksum](https://packages.debian.org/trixie/all/qemu-efi-aarch64/download)
- [EDK II ArmVirt platform](https://github.com/tianocore/edk2/tree/edk2-stable202502/ArmVirtPkg)
- [EDK II NOR interface definitions](https://github.com/tianocore/edk2/blob/edk2-stable202502/OvmfPkg/VirtNorFlashDxe/VirtNorFlash.h)
- [Arm PL011 register specification](https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5)
- [Apple GICv3](https://developer.apple.com/documentation/hypervisor/hv_gic_create(_:))
