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

The original ISO is 2,587,389,952 bytes. Only the selected files are retained;
HTTP byte-range extraction avoids storing the ISO, its approximately 1 GB VHDX,
and its optional packages. No installed disk or extra copy of the WIM is created.
The payload is not yet a bootable VM. The HVF backend now reaches an EDK II
UEFI Shell, but still needs boot media, block I/O and the Windows platform
interfaces described in
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
