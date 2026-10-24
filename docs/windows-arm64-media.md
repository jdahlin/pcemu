# Minimal Windows ARM64 test media

The local test payload is Microsoft Validation OS for ARM64, build
26100.9549, obtained from Microsoft's official Validation OS distribution.
Validation OS is a small Windows 11-based command-line environment for product
validation and bring-up. It is a Microsoft-licensed download, not open source
or a general-purpose Windows Server replacement.

Files are in ignored `media/validation-os-arm64/`:

- `ValidationOS.wim`: 281,080,081 bytes (about 268 MiB), already compressed.
- `license.rtf`: the license from the original media.
- `GenImage-readme.txt`: Microsoft's image-generation readme.
- `download.json`: original ISO URL/size, local SHA-256 hashes and WIM image
  metadata including verified ARM64 architecture and build.

The original ISO is 2,587,389,952 bytes. The WIM-only extraction retains the
selected files without storing the ISO, its VHDX or optional packages. A separate
VHDX download is now used for the verified boot described below. The WIM is
not expanded or duplicated.
The WIM payload alone is not a bootable VM. The original VHDX now
[boots to the administrator command prompt using NVMe](nvme.md). The HVF backend supplies the UEFI and platform interfaces described in
[Hypervisor backend](hypervisor.md). Do not pass the WIM to `--kernel`: that
option currently expects raw ARM64 instructions, not a Windows image.

To reproduce the download into a new directory:

```
python3 -m pip install --target build/media-tools pycdlib==1.15.0
python3 tools/fetch_validation_os.py --destination media/validation-os-arm64-new
```

The fetcher pins the upstream ARM64 ISO URL, validates exact HTTP ranges and
ETag continuity, limits transfer size, checks extracted file sizes, records
SHA-256 hashes and checks that the WIM XML identifies ARM64. The local hashes
are an audit of downloaded files, not an independent Microsoft signature check.
Existing destination directories are refused. Failed downloads may leave a
`.part` file in the new destination; there is no automatic resume.

## Why not Nano Server?

Microsoft's current `windows/nanoserver:ltsc2025` container manifest was inspected:
it publishes `windows/amd64`, not ARM64. A Windows container image supplies
user-mode files and relies on a compatible Windows host kernel; it is not a
standalone bootable Windows VM. It therefore cannot serve as our ARM64 HVF guest.
No Nano Server layers were downloaded.

Sources:
- [Microsoft Validation OS overview](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/validation-os-overview)
- [Validation OS terms and official ARM64 download](https://learn.microsoft.com/en-us/legal/windows/hardware/validation-os-license)
- [Windows container image model](https://learn.microsoft.com/en-us/virtualization/windowscontainers/about/index)
- [Nano Server LTSC 2025 manifest](https://mcr.microsoft.com/v2/windows/nanoserver/manifests/ltsc2025)

Driver inventory and small loader extraction: [virtio and Windows media](virtio.md).

The current ARM platform now publishes minimal ACPI and GOP, and exposes the
framework PMU needed by this loader. The loader-only probe reaches Windows Boot
Manager's missing-BCD status `0xc000000f`; this is not a Windows OS boot. See
[the optional loader smoke check](arm64-uefi.md#framework-pmu-and-windows-loader-check).

## Booting the original distribution without OS changes

The official ARM64 ISO is a distribution container: inspection found no El Torito
boot catalog and zero ISO boot records. It cannot be booted directly as an EFI
DVD. In addition to the WIM it contains `/ValidationOS.vhdx` (1,010,827,264 bytes),
which was discovered by inspecting the complete root directory.

Read-only HTTP-range inspection of that VHDX found a 32 GiB dynamic virtual disk
with GPT, an EFI system partition at LBAs 264192–342015, and a FAT32 filesystem
containing `\EFI\BOOT\BOOTAA64.EFI` (3,120,648 bytes). The original VHDX is thus
the input used for booting the preinstalled system without rebuilding
boot media or modifying Windows. Its kernel boots to the Validation OS command
prompt through [NVMe](nvme.md); the disk backend supports [direct VHDX read/write](vhdx.md). No full ISO or VHDX
was downloaded during this inspection, and the original WIM has not been modified.


To fetch only that original disk (about 964 MiB), without retaining the ISO:

```sh
python3 tools/fetch_validation_os.py --vhdx --destination media/validation-os-arm64-vhdx
make build/pcemu-hvf firmware-arm64
python3 tools/observe_validation_os.py
```

The VHDX fetch uses verified 8 MiB HTTP ranges with an 1152 MiB transfer
limit, records its SHA-256 and source ETag in `download.json`, and refuses an
existing destination. Keep the original license alongside the earlier WIM
payload. The observation runner attaches the downloaded VHDX writable and
uses a fresh private variable store. It retains serial/host logs, a GOP PPM
and a summary under ignored `build/validation-os-boot-*`. A successful runner
exit only means the bounded observation ended; inspect the artifacts to assess
Windows progress. No Windows OS boot is inferred from the process exit code.


### Original-disk bring-up result

The original 26100.9549 VHDX was downloaded and attached directly on the tested
Apple M4 host. Its EFI boot entry runs, reads the populated boot configuration,
and progresses beyond the loader-only missing-BCD milestone. The initial
platform exit was `ESR=0x62280503`, an `MRS X8, OSLSR_EL1` at
`PC=0xfffff800e11296a4`. The platform now supplies OS-lock status and control
(`OSLSR_EL1`/`OSLAR_EL1`), including cold-reset locked state and bit-zero
lock/unlock writes. Unknown system-register accesses still fail explicitly.
This is debug-lock bookkeeping, not external debug or breakpoint support.

A subsequent 15-second observation reached the Windows boot spinner, with
512 virtio requests and guest execution at `PC=0xfffff8029fa609ac`. The
Validation OS command prompt has not been verified. Boot animation and high
virtual addresses alone do not establish a usable Windows system.

Register definitions follow the Arm ARM's OSLAR_EL1/OSLSR_EL1 definitions
([Arm architecture register reference](https://developer.arm.com/documentation/ddi0487/latest/)).

The 60-second observation (`build/validation-os-boot-isy8fc3z`) reached the
Windows kernel's stop screen: **INACCESSIBLE_BOOT_DEVICE (0x7B)**. EFI storage
access succeeds (512 virtio requests), but the unchanged image lacks the
virtio driver needed after the firmware handoff. This identifies the next
storage milestone: expose a controller supported by an inbox ARM64 driver,
such as NVMe (`stornvme.sys`), or prepare separately licensed driver media.
The screenshot is `console.png` in that ignored observation directory.


### NVMe boot verified

Adding the original NVMe controller allowed the unchanged VHDX to boot through
its inbox `stornvme.sys` to an administrator command prompt. The 60-second GOP
capture in `build/validation-os-boot-qsn2t9ig/console.png` visibly shows
`Microsoft Windows [Version 10.0.26100.9549]` and `C:\Windows\System32>`.
This supersedes the earlier virtio-only 0x7B limitation. It does not establish
interactive keyboard input or clean Windows shutdown/restart support.

```sh
python3 tools/observe_validation_os.py --disk-controller nvme
```

See [NVMe implementation and validation](nvme.md) for details.
