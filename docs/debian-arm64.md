# Debian ARM64 bring-up

The existing `arm64-uefi` machine boots the Debian trixie ARM64 netboot installer
through modern virtio-blk PCI and reaches its language-selection screen on the
PL011 serial console. The verified kernel is `6.12.107+deb13-arm64`.
Installation to a system disk, virtio-net, installed-system reboot and SSH login
remain work in progress.

```sh
make build/pcemu-hvf firmware-arm64
python3 tools/smoke_debian_arm64.py
```

This downloads about 77 MiB of public Debian kernel/initrd assets on the first
run. The downloader pins their published SHA-256 checksums; a changed upstream
asset fails verification rather than silently using a different kernel. See
[Debian's netboot images](https://deb.debian.org/debian/dists/trixie/main/installer-arm64/current/images/netboot/debian-installer/arm64/)
and [published checksums](https://deb.debian.org/debian/dists/trixie/main/installer-arm64/current/images/SHA256SUMS).

The smoke check creates a private sparse 256 MiB FAT16 ESP under `build/`, stages
the EFI kernel and initrd, and uses `STARTUP.NSH` to pass the serial-console and
ACPI parameters. Kernel/initrd bytes allocate about 77 MiB; the remaining volume
is sparse. A private UEFI variable store and compressed disk COW isolate each
run. The runner links the repository's own devices and Hypervisor.framework;
no QEMU executable or QEMU device code runs.

`serial.log`, `host.log` and `summary.json` retain the actual boot evidence.
The check requires Linux/GIC/serial markers and the installer language prompt;
it does not claim successful disk installation or network access.

To prepare persistent bring-up media separately (the output must not exist):

```sh
python3 tools/prepare_debian_arm64.py
build/pcemu-hvf --machine arm64-uefi \
  --disk build/debian-arm64/netboot.raw --vars build/debian-arm64/my.vars \
  --timeout-ms 60000
```

The media builder and checksum cache stay under ignored `build/`. Do not attach
this installer as the target system disk or modify a parent while a VM uses it.
The current serial runner is bounded; an interactive input/control path and
long-running network service are still needed for installation and SSH testing.
