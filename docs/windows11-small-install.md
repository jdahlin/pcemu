# Small Windows 11 ARM64 installation

Research checked against Microsoft deployment documentation on 2026-10-26.
The `/Compact` deployment was completed and booted on this host on that date.
The other measures below remain recommendations until individually verified.
Disk-format compression is separate from changing the guest's Windows files.

## Fresh installation verified on this host

The Microsoft ARM64 ISO `Windows11_Client_arm64_en-us_26300_9457.iso` was copied
from the mounted archive to ignored `build/windows11-install/source.iso`; its
SHA-256 matched Microsoft's published
`8992f2d2cfd2fa647e579e9eb31fc21f34da8333c29b6170fde17d2ed6623e9c`.
WinPE applied Windows 11 Pro (index 3) with `DISM /Apply-Image /Compact
/CheckIntegrity /Verify` to a fresh sparse 64 GiB disk. The install report
reached 100%, installed the signed ARM64 VirtIO storage, network and RNG
drivers, and created EFI boot files. A non-formatting WinPE follow-up added the
local account files and fallback ARM64 EFI loader.

The installed disk booted without installation media. The first-logon report
at `build/windows11-install/retry-reports/report-1793029782373557000.log`
passed `tools/verify_windows_boot.py`: Windows build 26300.9457, PCEmu desktop
logon, started VirtIO storage/network/RNG, a DHCP lease, HTTP traffic and RNG
provider I/O. The guest posted an 800×600 desktop capture at
`build/windows11-install/desktop.png`. The normal 4 GiB, two-CPU VMBus/MSI
profile then reached the lock screen headless and powered off through the
guest shutdown service. Its capture is `build/windows11-qcow2/headless.png`.

The active, ignored files are `media/windows11-arm64/system.qcow2`, its writable
`system-live.qcow2` overlay, and `build/windows11-qcow2/installed-vars.bin`.
The standalone base has 64 GiB logical capacity and about 16.4 GiB host
allocation. `qemu-img check` passed, and `qemu-img compare` found every logical
byte identical to the stopped installation chain. The base uses zstd QCOW2
clusters; the live overlay is standard writable QCOW2. Start it with
`./run11.sh`; firmware and build dependencies are available locally.

This deployment retained the existing installer flow, which staged the 7.6 GB
WIM on the target volume and then deleted it. The deleted WIM's old clusters
may remain allocated in QCOW2. Because this host has ample space, that did not
block bringing up the guest. CompactOS was requested during DISM application,
but an in-guest `compact.exe /CompactOS:query` has not yet been captured;
hibernation, pagefile and component-store changes below have not been applied.
The stopped installation chain and failed first transfer remain in ignored
local media as recovery and diagnostic artifacts.

## Main finding in our current installer

`tools/windows/install.cmd` already uses `DISM /Apply-Image ... /Compact`.
However, it downloads the complete `install.wim` to the Windows target volume
(`W:\install.wim`) and deletes it after deployment. Deleting that file frees NTFS
space, but does not by itself remove its old bytes from the QCOW2 image. Those
bytes can remain allocated and are already compressed WIM data, so another
compression pass may save little. This is a likely source of avoidable image
size; it must be measured rather than counted as guaranteed savings.

For a fresh installation, keep the WIM off the target Windows volume. Microsoft
supports applying it from a network share or separate installation media:
[WinPE deployment media options](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe--use-a-single-usb-key-for-winpe-and-a-wim-file---wim).
For our HTTP-based installer, use a separate disposable installer/scratch disk
if a network share is not available. Remove that entire temporary disk after
installation. Do not leave a copied WIM or installation cache in the OS image.

## Recommended baseline

- Keep a sparse 64 GiB virtual system disk. Windows 11 requires at least 64 GB
  capacity; that does not require allocating 64 GiB of host storage.
- Apply a single official ARM64 edition with `/Compact`; install one language.
- Inject only the storage, network and other drivers actually used by this VM.
- Disable hibernation with `powercfg /hibernate off`. This also removes the
  hibernation-based fast startup path; the emulator uses ordinary cold boots.
- Keep a system-managed pagefile with 4 GiB guest RAM. Arbitrarily limiting or
  disabling it can cause commit-limit failures and harm application reliability.
- Inventory provisioned apps, optional features and capabilities. Remove only
  clearly unused items through supported servicing commands; preserve Windows
  Update, Defender, the shell, servicing and the Store framework/dependencies.
- After updates and device validation, run `DISM /Online /Cleanup-Image
  /StartComponentCleanup`. `/ResetBase` can save further space but removes the
  ability to uninstall updates already in the image; use only for a deliberately
  sealed/tested base with a separate recoverable checkpoint.
- Keep one immutable zstd-compressed QCOW2 base and one standard writable QCOW2
  layer. Repack offline after cleaning, rather than accumulating abandoned
  experiments or copying installation payloads into the system volume.

## Measures that need separate decisions

Disabling reserved storage can free Windows-visible capacity but removes update
headroom; it does not guarantee an equal reduction in physically allocated host
image bytes. Start with it enabled, measure actual allocation, and reconsider
only if necessary. Likewise, deleting WinRE trades away built-in recovery and
should not be bundled into generic cleanup. Feature disabling alone does not
necessarily remove payloads on Windows client editions.

Compact OS compresses the operating-system files and continues to permit
Windows Update to replace individual files. QCOW2 zstd compresses host disk
clusters independently. They work at different layers; their combined savings
and boot-time cost must be measured, not assumed to multiply.

## Validation and measurements

Record the WIM edition/index, installed languages, CompactOS query, pagefile and
hibernation configuration, DISM component-store analysis, Windows used/free
space, and actual host allocation for both QCOW2 files. Check installed drivers,
networking, updates and two clean headless boot/shutdown cycles. Compare every
allocated guest byte for a lossless representation-only conversion. Any later
filesystem free-space reclamation requires a separate validation, because bytes
in filesystem free space may intentionally change.

## Microsoft sources

- [Compact OS, deployment and footprint considerations](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/compact-os)
- [Windows 11 requirements, including VM storage](https://learn.microsoft.com/en-us/windows/whats-new/windows-11-requirements)
- [Component-store cleanup and ResetBase](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/clean-up-the-winsxs-folder)
- [Reserved storage controls and update purpose](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/dism-storage-reserve)
- [Hibernation controls](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/powercfg-command-line-options)
- [Pagefile commit-limit considerations](https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/introduction-to-the-page-file)
- [Feature servicing and client payload limitations](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/enable-or-disable-windows-features-using-dism)
