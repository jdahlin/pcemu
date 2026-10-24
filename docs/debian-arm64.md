# Debian ARM64 bring-up

The existing `arm64-uefi` machine boots the Debian trixie ARM64 netboot installer
through modern virtio-blk PCI and reaches its language-selection screen on the
PL011 serial console. The verified kernel is `6.12.107+deb13-arm64`.
Virtio-net PCI is connected to the socket gateway and the installer configures
its `enp0s2` interface through DHCP. Installation to a system disk, installed-system
reboot and SSH login remain work in progress.

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
`--serial-stdio` connects stdin to the PL011 console; Ctrl-] exits and saves
private state. `--run-forever` keeps the guest running until power-off, reset or a
host SIGINT/SIGTERM. A finite observation timeout remains the default.

Use `--extra-disk EMPTY_RAW --extra-disk-overlay PRIVATE_COW` to attach a separate
virtio installation target. The boot disk is PCI slot 0 (SPI 48), target slot 1
(SPI 49), and virtio-net slot 2 (SPI 50); ACPI and device-tree interrupt maps agree.
Each disk retains an immutable parent and independently locked writable delta.
`--network user --ssh-port 2222` enables DHCP, DNS and socket networking plus a
loopback-only `127.0.0.1:2222` forward to guest port 22. The network uses
`10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`. A `--network-capture FILE`
option records Ethernet traffic for diagnosis. No privileged network service is
required. The vCPU observation slice is capped at 2 ms with networking enabled,
so host socket input can wake an otherwise idle guest.

## Network device under construction

The selected device is modern virtio-net PCI (`1af4:1041`), using the shared
virtio PCI transport. The portable `VirtioNet` device provides one RX/TX
split-queue pair with 256 descriptors each, MTU 1500, MAC/link reporting,
transmit checksum offload and EVENT_IDX. It drains a TX batch per notification
and signals once for that batch. Packed rings, indirect descriptors, multiple
queue pairs and segmentation offload are not advertised yet. No throughput or
CPU-overhead comparison has been established.

The model validates every descriptor and DMA range before packet access and
bounds pending outgoing frames. Portable checks cover scatter/gather receive,
checksum completion, event suppression, 16-bit ring wrapping, malformed queues
and reset. Run the portable model checks with:

```sh
make test-virtio-net
```

The real Debian installer has enumerated both virtio disks and configured DHCP
through this device. Installer completion and an authenticated SSH session remain
separate required checks.
