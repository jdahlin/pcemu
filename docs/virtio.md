# Virtio block storage on ARM64

`arm64-uefi` now has modern virtio-blk over PCI (default) or MMIO. Real EDK II
finds the disk, reads its FAT filesystem and boots an original ARM64 EFI probe.
Guest file writes survive a new VM without changing the disk parent. This is
firmware/device validation, not a claim that Windows boots yet.

```
make smoke-virtio
```

For an existing immutable base disk:

```
build/pcemu-hvf --machine arm64-uefi --disk PATH_TO_BASE \
  --vars build/my-arm-vm/vars.bin --until-text 'Shell> '
```

`--disk IMAGE.vhdx` opens VHDX directly for reading and writing by default; no
COW file is created. See [native VHDX storage](vhdx.md) for format limits.
For RAW and PCEMUCOW inputs, `--disk` remains the immutable parent and the default
private compressed COW file is `<vars-path>.pcd`. Explicit `--disk-overlay PATH`
selects an optional private overlay for any supported parent, including VHDX.
Reopening an overlay requires its saved parent to match. Opening the parent
itself as the overlay is rejected. Variable stores and disk overlays have
separate advisory locks. Never modify an attached parent using other tools.
`--virtio-transport pci|mmio` chooses which interface exposes this one disk;
RAM and the disk queue implementation are shared.

## Device and transport contracts

- Virtio 1.x modern device, device ID 2; MMIO version 2. No legacy/transitional
  register interface. One split queue, up to 256 direct descriptors.
- Advertise VERSION_1, BLK_SIZE (512 bytes), FLUSH, SIZE_MAX (128 KiB per segment)
  and SEG_MAX (126 data segments). No indirect/packed rings, EVENT_IDX, multiqueue,
  discard, write-zeroes or IOMMU features are advertised.
- IN, OUT, FLUSH and GET_ID requests. Scatter/gather may split the header and
  combine data/status. Headers, ring entries and config data are little-endian.
- Drain the available batch on notification; publish used entries and lengths.
  Honour NO_INTERRUPT, interrupt acknowledgement and 16-bit index wrapping.
- Validate queue size/alignment, every descriptor index, cycles, flags, ordering
  and every DMA range before disk/data access. DMA is limited to mapped guest RAM.
  Malformed chains request device reset; the current runner reports a failure.
  Out-of-range disk requests and transfers over 1 GiB return IOERR. Valid
  transfers stream through a host buffer of at most 128 KiB. This also supports
  EDK II loading large EFI images without negotiating SIZE_MAX.
- Completion is synchronous while the single vCPU is stopped. No host syscall
  occurs per guest instruction or per descriptor. Disk I/O uses the
  shared synchronous storage backend; async I/O and multiple vCPUs are future work.

PCI endpoint `00:00.0` exposes vendor `1af4`, device `1042`, revision 1, subsystem
`0042`, a vendor capability list and one 16 KiB 32-bit MMIO BAR. The BAR contains
common configuration, notify (multiplier 4), read-to-clear ISR and device
configuration regions. Memory/bus-master enables and legacy INTx masking are
implemented. MSI-X and PCIe extended capabilities are not implemented.

The single PCI root bridge uses ECAM at `0x3f000000`, bus 0, with a 32-bit memory
window from `0x10000000` to `0x3effffff`. INTx routes to GIC SPI 48 through the
device-tree interrupt map. The optional MMIO device occupies `0x0a000000` and
uses the same SPI. Only one transport presents the disk at a time. With no disk,
both interfaces advertise no device. The platform identity is now `arm-virt-v3`;
there is still no ARM checkpoint format to migrate.

`src/disk.hpp` separates storage declarations from the x86 machine. `disk.cpp`
is now usable without an x86 CPU or device implementation. Its existing
CPU-transfer bridge lives in `disk_cpu.cpp`; the COW format and legacy API are
unchanged. The hypervisor runner links this shared storage and zstd, not the
x86 execution core. `src/virtio_blk.cpp` owns queues/block requests and
`src/virtio_pci.cpp` adapts the same device to PCI. Its `VirtioPciDevice`
transport accepts register callbacks, a device identity and a queue count,
so network and block devices can share modern PCI capability/INTx handling.
Each queue has its own notification offset (multiplier 4).

## End-to-end checks and space use

`tools/build_uefi_probe.py` assembles the original position-independent EFI probe
and wraps it in a small ARM64 PE32+ image. `tools/make_uefi_test_disk.py` formats
sparse 64 MiB FAT16/MBR ESP test media without mounting host filesystems.
These are firmware test fixtures, not Windows installation media.

`make smoke-virtio` performs four fresh VM boots:

1. Boot `EFI/BOOT/BOOTAA64.EFI` through virtio-PCI; require actual EFI program output.
2. Boot the same probe through virtio-MMIO.
3. Use a separate tiny data-only fixture to reach Shell, read `PROBE.TXT`, write
   `WRITE.TXT`, and read the written content through the firmware FAT/Block I/O stack.
4. Restart using the same private PCI COW overlay and read the saved file again.

Separate boot/data fixtures avoid the firmware's setup menu after a successful
EFI application's return. Both base hashes must remain unchanged. Logs and
`summary.json` live in `build/virtio-smoke-*`. A verified run used about 2.3 MiB
of allocated base storage, despite two 64 MiB logical disks, and an **832-byte**
compressed write delta. No guest RAM dump or full disk copies are retained.

`make test-hvf` also runs 565 block/PCI checks, including index wrap, split
headers, malformed DMA/cycles, oversize I/O, capability discovery, BAR sizing,
PCI queue setup, ISR read-to-clear, INTx masking and a real compressed COW parent.
The existing snapshot/COW suite also passes after separating the storage code.
The virtio suite also passes with AddressSanitizer and UndefinedBehaviorSanitizer.

## Windows Validation OS

The downloaded ARM64 WIM contains `acpi.sys` and `stornvme.sys`, but no
`viostor.sys`, `vioscsi.sys` or `netkvm.sys`. Verify the exact image and extract
only the small loader payload with:

```
python3 tools/inspect_validation_os.py --wimlib PATH_TO_WIMLIB_IMAGEX
```

`wimlib-imagex` is an optional offline tool, not an emulator dependency; it is
available as Homebrew's `wimlib`. The report, ARM64 `bootmgfw.efi` and original
BCD template are stored under `build/validation-boot/`. The WIM is not expanded
or duplicated. `--make-loader-probe` optionally creates diagnostic EFI-only
media; it has no populated Windows BCD/ramdisk boot configuration and is not a
working Windows boot disk. An attempted Windows-loader-only boot currently
raises a guest synchronous exception; its exact dependency is not diagnosed.

The virtio-win project includes ARM64 viostor/vioscsi build packaging; viostor
uses the PCI transport, hence PCI is our default. That does not establish that
an arbitrary packaged driver is signed, compatible with this Validation OS, or
installed in our WIM. Those must be verified before native Windows disk I/O.
For RAM-disk boot, UEFI can first load the WIM using its own virtio driver;
Windows then runs from RAM. Either path still needs Windows-compatible platform
interfaces and validation. GOP and a minimal ACPI handoff are available; Windows
boot configuration and OS/device compatibility still need validation.
TPM/Secure Boot and the complete Windows 11 platform remain future work.

References:
- [OASIS Virtio 1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
- [EDK II virtio-MMIO transport](https://github.com/tianocore/edk2/tree/edk2-stable202502/OvmfPkg/Library/VirtioMmioDeviceLib)
- [EDK II modern virtio-PCI](https://github.com/tianocore/edk2/blob/edk2-stable202502/OvmfPkg/Virtio10Dxe/Virtio10.c)
- [Virtio-win ARM64 packaging](https://github.com/virtio-win/kvm-guest-drivers-windows/blob/master/win11_arm64.ddf)
- [Virtio-win viostor PCI interface](https://github.com/virtio-win/kvm-guest-drivers-windows/blob/master/viostor/virtio_stor.h)


The original Validation OS VHDX reaches Windows' `INACCESSIBLE_BOOT_DEVICE`
stop screen on the virtio endpoint. This particular image has no installed
VirtIO storage driver; Windows supports VirtIO when suitable drivers are
installed. The same unchanged disk now boots to its command prompt through
our [NVMe controller](nvme.md), using the inbox `stornvme.sys` driver.
