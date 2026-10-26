# VirtIO devices for guest tools

PCEmu supplies the host devices; signed Windows drivers come from the existing
ARM64 VirtIO packages. A staged driver package, a started device, and a running
user service are separate milestones.

## Serial and guest-agent channel

`pcemu-hvf --virtio-serial` exposes a modern PCI serial device (1AF4:1043) in
slot 5, INTx SPI 53 or MSI-X. It implements one generic port named
`org.qemu.guest_agent.0`, four split queues, and the multiport device/port ready,
port name, and host/guest open-state protocol. It is not marked as a console.
The Windows name is `\\.\Global\org.qemu.guest_agent.0`; Linux uses
`/dev/virtio-ports/org.qemu.guest_agent.0`.

Adding `--guest-agent-socket /private/path/agent.sock` enables the device and
creates a nonblocking Unix stream listener, mode 0600. The parent directory must
exist. Existing paths are refused, and closing the VM removes only its own
socket inode. The client is a host management program; the other end must be a
real agent or test process in the guest. PCEmu forwards opaque bytes without
parsing or inventing guest-agent replies. Input/output buffering is bounded,
full output applies virtqueue backpressure, reconnects signal PORT_OPEN, and
idle/closed receive queues are not walked. No TCP listener is created.

The pinned VirtIO ISO has a signed ARM64 vioserial driver but no native ARM64
QEMU guest-agent MSI. Exposing this channel does not install a guest service or
provide clipboard/shared-folder commands. Those require a compatible guest
program and their corresponding protocols.

The reusable modern split-ring core checks negotiated features, queue geometry,
ring overruns, descriptor cycles/direction, DMA bounds, response sizes and active
queue changes. Indirect descriptors, packed rings and EVENT_IDX are not
advertised. Each queue retains its own indices and interrupt vector. PCI also
forwards device configuration writes and the configuration generation byte.

## Validation

```sh
make test-virtio-serial
./tools/build.sh virtio-serial-sanitized-tests
./build/virtio-serial-sanitized-tests
PATH=/opt/homebrew/opt/llvm/bin:$PATH python3 tools/build_windows_guest.py
python3 tools/build_arm_platform.py --cpus 2 --ram-mb 4096 --hyperv --vmbus --msi
python3 tools/smoke_windows_virtio_serial.py
```

The headless runner uses the already available official ARM64 boot WIM/ISO,
NetKVM and vioserial packages. It never attaches an installed Windows disk.
An original ARM64 process opens the named port through the signed Windows
driver, sends a readiness marker, checks sixteen binary bytes sent in two host
writes, and returns a success marker. The report is posted over the existing
VirtIO network; Windows PE powers off. Device counters establish actual DMA
and MSI-X delivery. Successful generated WIM/bootstrap files are removed;
logs, report, source hashes and result JSON remain in `build/windows-serial-*`.
The 2026-10-26 native run passed with 16 received bytes, 45 transmitted bytes,
18 queue completions and 11 MSI-X messages; Windows PE powered off normally.
[Retained evidence](measurements/windows-virtio-serial-2026-10-26.json).
Failures retain their media. `--keep-artifacts` retains successful media too.

Native tests also cover scatter/gather receive buffers, TX backpressure,
closed-port buffer detachment, descriptor corruption, 16-bit index wraparound,
socket permissions, collision refusal and a real socket exchange. They run
under AddressSanitizer and UndefinedBehaviorSanitizer.

Protocol references: [OASIS VirtIO console](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html),
[upstream Windows vioserial](https://github.com/virtio-win/kvm-guest-drivers-windows/tree/master/vioserial),
and [QEMU guest-agent port naming](https://github.com/qemu/qemu/blob/master/qga/main.c).
