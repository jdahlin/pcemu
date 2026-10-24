# Minimal Debian ARM64 server

Debian 13.7 is installed on a virtio disk and boots through the existing
`arm64-uefi` profile. Authenticated SSH login, virtio networking and a file saved
through SSH surviving a full shutdown/new VM process have been verified.
The installation contains 232 packages, OpenSSH and no desktop; the ext4 root
used about 916 MiB at verification. The kernel is `6.12.107+deb13-arm64` and
OpenSSH is `1:10.0p1-7+deb13u4`.

See the [platform audit and integration roadmap](debian-platform-audit.md) for
observed kernel warnings and prioritized host/guest improvements.

## Run the installed server

The local VM is under ignored `media/debian-arm64/`. It contains the sparse
16 GiB `base.raw`, compressed writable `system.pcd`, private `vars.bin`, SSH
key pair, pinned `known_hosts` and boot/verification logs. Keep the base with its
delta. This is the repository's native COW format, not QCOW2. Disk capacity is
virtual; host allocation grows with data rather than reserving 16 GiB.

```sh
./run-debian.sh
```

The launcher builds/signs the native runner and verifies firmware assets before
starting the existing disk. It does not reinstall Debian. Ctrl-] saves host state
and exits; prefer a clean guest shutdown when possible. A running instance holds
exclusive disk/variable locks, so stop it before starting another.

From another terminal in this checkout:

```sh
ssh -i media/debian-arm64/id_ed25519 -p 2222 \
  -o UserKnownHostsFile=media/debian-arm64/known_hosts pcemu@127.0.0.1
```

SSH uses the generated private key; password authentication is disabled. The
`pcemu` account can use `sudo /usr/sbin/poweroff` and `sudo /usr/sbin/reboot`
without a password for lifecycle checks. Other sudo commands are not enabled by
that rule. A guest reset ends the current runner; start it again to complete a
reboot. `DEBIAN_VM_DIR` and `DEBIAN_SSH_PORT` override the launcher's defaults.

The installed-system boot attaches **only the system disk**, with no installer
or host-supplied kernel/initrd. Linux reports `/dev/vda3` as the ext4 root, the
`virtio_blk` storage driver and `virtio_net` for `enp0s2`. DHCP supplies
`10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`. The SSH forward binds only
`127.0.0.1:2222`; networking needs no privileged host service. The gateway
currently supplies IPv4.

## Installation and verification

The installed server was created using Debian's public ARM64 network installer,
a separate 16 GiB sparse target disk, a private preseed and a generated SSH key.
Only the `ssh-server` task plus `openssh-server`/`ca-certificates` were requested.
The default atomic partition recipe requires more than 8 GiB; using a larger
sparse target avoids allocating that capacity on the host. The preseed selected
`/dev/vdb`, installed GRUB's removable EFI fallback, and configured the serial
console. Installation finished with an actual guest PSCI power-off.

The installer media is available reproducibly:

```sh
make build/pcemu-hvf firmware-arm64
python3 tools/prepare_debian_arm64.py
```

The downloader pins the published SHA-256 checksums of the kernel and initrd.
A changed upstream asset fails verification. See
[Debian's netboot images](https://deb.debian.org/debian/dists/trixie/main/installer-arm64/current/images/netboot/debian-installer/arm64/)
and [published checksums](https://deb.debian.org/debian/dists/trixie/main/installer-arm64/current/images/SHA256SUMS).
The sparse 256 MiB FAT16 ESP stages the EFI kernel/initrd and `STARTUP.NSH`.
Downloaded bytes and VM files remain ignored. No QEMU executable or QEMU device
code runs; the runner links the repository's devices and Hypervisor.framework.

For a new installation, attach the installer with `--disk` and a **different**
empty target with `--extra-disk TARGET --extra-disk-overlay PRIVATE_COW`.
Enable `--network user`, and use `--serial-stdio --run-forever` to drive the
installer. The existing installed disk must not be used as a new install target.
The boot disk is PCI slot 0 (SPI 48), target slot 1 (SPI 49), network slot 2
(SPI 50), entropy device slot 3 (SPI 51); ACPI and device-tree maps agree. Each writable delta is independently
locked. `--network-capture FILE` records Ethernet traffic for diagnosis.

`build/debian-arm64-installed/summary.json` and its logs record the local
installation, installed-disk-only boot, authenticated SSH checks, drivers and
file survival. Private keys and preseed credentials are not committed.

## Regression checks and network contract

```sh
make test-hvf
make build/network-tests && build/network-tests
python3 -m unittest discover -s tests -p test_debian_media.py
python3 tools/smoke_debian_arm64.py
```

The last command downloads about 77 MiB on its first run and verifies that the
real installer reaches language selection using a fresh private disk/variables.
It is a quick firmware/kernel regression, separate from the recorded complete
installation/SSH test.

Modern virtio-net PCI (`1af4:1041`) shares the block device's PCI transport.
The device provides one RX/TX split-queue pair with 256 descriptors each,
MTU/MAC/link reporting, transmit checksum offload and EVENT_IDX. TX batches
complete with one interrupt decision; the host gateway sends bounded TCP
windows rather than waiting for a per-packet ACK. The network-enabled vCPU
slice is capped at 2 ms so host input can wake an idle guest.

DMA validation, scatter/gather receive, checksum completion, event suppression,
16-bit ring wrap, malformed queues and reset have portable tests. Socket tests
cover bidirectional loopback forwarding, partial ACKs and retransmission.
Packed rings, indirect descriptors, multiple queue pairs, segmentation offload,
MSI-X and IPv6 are not advertised. No comparative throughput or CPU-overhead
claim has been established.
