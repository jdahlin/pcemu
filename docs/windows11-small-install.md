# Small Windows 11 ARM64 installation

Research checked against Microsoft deployment documentation on 2026-10-26.
This is the proposed installation profile; it has not yet been applied in a
fresh installation. Disk-format migration and image compression are separate
from changing the guest's Windows installation.

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
