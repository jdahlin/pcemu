# ARM64 UEFI platform

`arm64-uefi` boots real EDK II firmware to its built-in UEFI Interactive Shell
using Hypervisor.framework. Serial command execution and nonvolatile variables
survive a complete VM teardown and restart. The platform also [boots EFI applications from virtio disks](virtio.md).
The [minimal Debian ARM64 server](debian-arm64.md) boots from its installed
virtio disk and accepts authenticated SSH over modern virtio-net.
[NVMe storage](nvme.md) boots Windows Validation OS to its administrator command
prompt.

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

## Guest platform contract: arm-virt-v3

| Address | Device |
|---|---|
| `0x00000000` | Read/execute firmware ROM, reset entry at zero |
| `0x04000000` | 768 KiB NOR variable store, guest writes trapped |
| `0x08000000` | Hypervisor.framework GICv3 distributor |
| `0x080a0000` | GICv3 redistributor for the single vCPU |
| `0x09000000` | PL011 serial console, GIC SPI 33 |
| `0x0a000000` | Optional virtio-blk MMIO device |
| `0x10000000`–`0x3effffff` | PCI memory window |
| `0x3f000000` | PCI ECAM, bus 0 |
| `0x40000000` | 2 GiB RAM; device tree initially at RAM base |

The CPU starts at EL1h with translation disabled and DAIF masked. EDK II initializes
its MMU, interrupt controller and architectural timer. The physical timer uses
PPI 30; GICv3/timer operation is managed by Hypervisor.framework. Firmware output
and a progressing shell countdown verify the current timer path. No EL3 monitor
or nested virtualization is used. The HVC conduit implements a PSCI 0.2 subset: version, CPU_ON, CPU_OFF,
AFFINITY_INFO, MIGRATE_INFO_TYPE and standby with immediate wakeup. CPU_ON/OFF
operate on live vCPU state; the final online CPU cannot be taken offline.
Power-down suspend is rejected. SYSTEM_OFF stops the VM;
SYSTEM_RESET ends the current platform with `state=reset-requested`. The CLI
flushes disks and UEFI variables, destroys the old VM/devices/sockets and boots
a fresh platform in the same process. Power-off still exits. The timeout covers
the entire run, including reboots; signals and a matched `--until-text` stop
without another boot. [SMP supports 1–8 CPUs](arm64-smp.md). This does not provide deep-idle power management.

`platform/arm64/pcemu-virt.dts` supplies the versioned hardware description.
The runner expects that compiled tree and its fixed RAM map; accepting a path to
an arbitrary DTB does not make arbitrary hardware layouts supported. Device tree configures firmware. The OS receives ACPI through the UEFI
configuration table. Our original tables describe the CPU, GICv3, architectural
timers, PCI ECAM/window/INTA and PL011 console/debug port. EDK II installs
and relocates them using `etc/table-loader`; it owns the final RSDP/XSDT.
`make acpi-arm64` regenerates these small assets; `--acpi-dir DIRECTORY` selects
an alternate generated set. This does not make alternate hardware maps supported.

The source namespace is `platform/arm64/dsdt.asl`. Its compiled AML and source
hash are in `dsdt-aml.json`; no compiler is needed for normal builds. To check the
compiled bytes, run `python3 tools/build_arm_acpi.py --iasl /path/to/iasl`.
The UART exposes its memory resource and level-high GIC interrupt 33.

`src/arm_virt.cpp` owns firmware loading, execution and variable persistence.
It uses the separate `HvfMachine` execution backend, not the x86 `Machine`.
`src/arm_virt_devices.cpp` registers MMIO regions and PCI endpoints using
`MmioSpace` and `PciEcamBus`. Device models live in separate files:
`arm_pl011.*`, `arm_pl031.*`, `arm_nor_flash.*`, `arm_psci.*`, `arm_os_lock.*`,
`arm_fw_cfg.*` and `arm_ramfb.*`. See [device composition](debian-device-model.md).
These remain deliberately limited firmware device models:

- PL011 transmit/receive, selected control registers and PrimeCell IDs.
  Input is a bounded host queue. Masked RX, receive-timeout and empty-TX level
  interrupts reach GIC SPI 33. FIFO thresholds and DMA are not implemented.
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

There is no TPM or Secure Boot implementation. Secure Boot is not enabled for the
shell milestone. Windows Validation OS boots with the [NVMe controller](nvme.md); interactive
input and clean Windows shutdown/restart still need validation.

## Verification

`make test-hvf` includes backend ROM/GIC tests, 74 portable NOR/UART/MMIO-load/fw_cfg/PSCI checks and 565 virtio block/PCI checks and
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

`make smoke-arm-acpi` boots our original EFI probe from a sparse virtio disk.
It follows the firmware-published ACPI configuration pointer, checks the root
and leaf table checksums, validates the FADT/DSDT link, exercises real HVC
version/CPU_ON/AFFINITY_INFO calls, then requires SYSTEM_OFF to stop the VM.
The table payload is about 1.2 KiB and relocation commands use 2.5 KiB.
The 64 MiB logical test disk allocates about 1.2 MiB, with a 100-byte COW.

Table layouts follow the [ACPI 6.5 specification](https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model.html);
the handoff follows EDK II's [table-loader definitions](https://github.com/tianocore/edk2/blob/edk2-stable202502/OvmfPkg/Include/IndustryStandard/QemuLoader.h).
PSCI identifiers and return values follow Arm's
[PSCI interface definitions](https://github.com/ARM-software/arm-trusted-firmware/blob/master/include/lib/psci/psci.h).
This is a platform bring-up milestone, not proof of Windows compatibility.

`--cpu-state` prints a small diagnostic record: PC, ESR/ELR/FAR/SPSR_EL1 and
64 instruction bytes around ELR and the first firmware-reported exception PC.
Only guest-RAM addresses are read; no RAM image is written to disk. The reported
PC helps diagnose an original guest fault when the firmware exception handler
itself subsequently faults. The original Validation OS loader fault was `MRS PMCCNTR_EL0` (performance
cycle counter). Matching guest bytes to the existing ARM64 `bootmgfw.efi`
identified it. Exposing the PMUv3 feature now lets the framework's GIC path
handle that register, and the loader reaches its expected missing-BCD error
instead of a synchronous CPU exception. This still does not boot Windows.

## Framework PMU and Windows loader check

The GIC-enabled backend exposes `ID_AA64DFR0_EL1.PMUVer=1` and explicitly
disables guest EL2. On the tested Apple M4/macOS 26.6 host, framework GIC mode
handles cycle-counter reads internally. A bare-metal regression reads the
counter twice around a short compute loop and requires exactly one userspace
exit (the final HVC), demonstrating that those reads do not enter our dispatcher.
The counter is supplied by the framework; its frequency has not been calibrated
as a measure of guest CPU cycles. It is not an instruction count. Counter enable/disable and event profiling are not
claimed: the framework counter continued advancing after PMCNTENCLR in our probe.
No PMU interrupt is advertised in MADT. There is no linked or unused software
PMU model. Older supported hosts need the same runtime checks before relying
on this Windows bring-up path.

For the optional Windows loader milestone, first create the diagnostic disk:

```sh
python3 tools/inspect_validation_os.py --make-loader-probe
make smoke-windows-loader
```

The smoke check reuses that immutable sparse disk with a private COW, requires
Windows Boot Manager to report `\EFI\Microsoft\Boot\BCD` and `0xc000000f`,
and rejects a firmware-reported CPU exception. It downloads no proprietary media
and does not run as part of the standard tests. Current result: the boot manager
executes and reports missing BCD; BCD, boot.sdi, WIM media setup and Windows OS
boot remain unverified. The private disk delta was 102 bytes in the measured run.

Feature-field and PMU register definitions follow the
[Arm Cortex-A57 reference manual](https://documentation-service.arm.com/static/5e906b9fc8052b1608760c7b);
framework controls follow the installed Apple Hypervisor SDK's `hv_vm_config.h`
and `hv_vcpu_types.h`. The GIC-mode behavior above is backed by the runtime test,
not assumed from the EL2-disabled PMU documentation alone.

Native VHDX images can be attached directly with `--disk IMAGE.vhdx`; writes go
to that file unless `--disk-overlay` is explicitly supplied. See
[VHDX storage](vhdx.md) for supported layouts and validation.

The EL1 debug OS-lock model also accepts Linux's `MSR OSDLR_EL1, XZR` during
early CPU initialization. OSDLR bit zero is sticky until VM recreation; once
set it prevents OS-lock changes. This does not add software breakpoint execution.

The original Windows Validation OS VHDX now reaches its administrator command
prompt with `--disk-controller nvme`; see [NVMe storage](nvme.md) for the
verified milestone, commands and current limitations.

## Real-time clock

A host-backed PL031 at `0x09010000` (SPI 34) is described in the firmware device
tree. EDK II discovers it and supplies EFI time services; an ACPI Linux guest
uses `rtc-efi` without needing a second native RTC device in the DSDT. Time is
UTC, follows host wall time including sleep, and guest SetTime changes only a
per-VM offset. That offset is not persisted across a new VM process. Alarm match,
mask, clear and level interrupts are emulated; delivery is polled at most once
per second while the runner executes. This does not implement host wake from
sleep or an NTP client in the guest.

Verified with Debian 13.7: `rtc0` registers as `rtc-efi`, the cold-boot system date
and RTC agree with the host (2026-10-24), SSH works and no services fail.

RTC polling avoids host clock reads while the alarm is masked, and updates HVF's
interrupt line only when its level changes. No time daemon or network clock
service is required for the RTC; the installed Debian guest uses the RTC alone.

`make smoke-arm-reboot` boots an EFI probe that writes a nonvolatile marker,
calls ResetSystem, then reads the marker on the second boot and powers off.
It verifies one reset and two firmware boots in a single runner process.

## Host sleep and virtual time

HVF defines `CNTVCT_EL0 = mach_absolute_time() - vtimer_offset`. The host's
absolute clock excludes system sleep, while `mach_continuous_time` includes it.
The backend samples their difference once per active second and advances the
virtual counter by newly observed sleep time before entering the vCPU again.
The GIC/virtual timer then sees elapsed deadlines. There is no guest daemon,
network time service or additional host polling thread.

Sampling is bracketed to reject host scheduling delays; differences below 10 ms
are ignored. Time is advanced once per observed sleep interval and never moved
backwards. Ordinary process pauses need no adjustment because absolute time
already advances while the process is stopped. The PL031 RTC continues to read
host wall time. Manual host wall-clock changes and physical-counter-based guest
clocks are outside this virtual-counter correction.

The signed HVF self-test injects sleep samples into a real vCPU and checks
CNTVCT against host clock brackets, including jitter, prior sleep, repeated and
backwards samples. Separate cases verify an armed virtual timer expires after
a simulated 20-second system sleep, both through an HVF timer exit and the
framework GIC's pending PPI. This tests the offset/timer mechanism;
physical Mac sleep/wake has not been exercised automatically.
