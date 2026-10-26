# VirtIO devices for guest tools

PCEmu supplies the host devices; signed Windows drivers come from the existing
ARM64 VirtIO packages. A staged driver package, a started device, and a running
user service are separate milestones.

## Serial and guest-agent channel

`pcemu-hvf --virtio-serial` exposes a modern PCI serial device (1AF4:1043) in
slot 5, INTx SPI 53 or MSI-X. By default it implements one generic port named
`org.qemu.guest_agent.0`, four split queues, and the multiport device/port ready,
port name, and host/guest open-state protocol. It is not marked as a console.
The controller also supports up to eight independent named ports (two data
queues per port plus the shared control pair), including a separate SPICE agent
channel. Open state, receive buffers and transmit backpressure are per port.
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

## Balloon memory coordination

`--virtio-balloon` exposes PCI 1AF4:1045 in slot 6, INTx SPI 54 or MSI-X.
`--balloon-target-mb N` requests N MiB of guest memory for the balloon (zero by
default). Inflate/deflate PFNs are validated before accounting changes, actual
size is written by the guest, and the third queue implements host-polled memory
statistics. Missing service-provided statistics remain unknown; the Windows
balloon user service has not been installed or started by this validation.

Host discard hints are permitted only after negotiating MUST_TELL_HOST and only
for complete host pages whose constituent 4 KiB guest pages are all ballooned.
The Darwin backend uses MADV_DONTNEED; accepted hints do not establish an equal
reduction in resident host memory. A driver that omits MUST_TELL_HOST can still
coordinate guest memory, but PCEmu issues no discard hints for it.

The signed Windows 0.1.302 balloon package sends arrays of 64-bit PFNs instead
of the specified 32-bit entries. The normal device correctly rejects the zero
upper half as an invalid guest page. For this specific package, opt in to
`--balloon-pfn64`; it changes PFN parsing on both inflate and deflate queues and
continues to validate all guest addresses. It is not advertised as a VirtIO
feature or automatically inferred. Remove this option with a corrected driver.
[Upstream fix 9f96702d8206](https://github.com/virtio-win/kvm-guest-drivers-windows/commit/9f96702d8206)
was merged on 2026-10-26, after the pinned signed package was built.

The native WinPE test with this option observed exactly 16,384 inflated pages
(64 MiB), guest-written actual=16,384, and all 16,384 pages returned on shutdown.
No discard hints were issued because this Windows driver does not negotiate
MUST_TELL_HOST. [Evidence](measurements/windows-virtio-balloon-2026-10-26.json).

## Validation

```sh
make test-virtio-serial
./tools/build.sh virtio-serial-sanitized-tests
./build/virtio-serial-sanitized-tests
PATH=/opt/homebrew/opt/llvm/bin:$PATH python3 tools/build_windows_guest.py
python3 tools/build_arm_platform.py --cpus 2 --ram-mb 4096 --hyperv --vmbus --msi
python3 tools/smoke_windows_virtio_serial.py
python3 tools/smoke_windows_virtio_serial.py --balloon
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

## GPU 2D and installed Windows validation

`--virtio-gpu` exposes PCI 1AF4:1050 in slot 7, INTx SPI 55 or MSI-X.
The control and cursor queues implement display information, EDID, 2D resource
creation, bounded scatter/gather backing, rectangular transfers, scanout,
flush, fences and a 64×64 alpha cursor. Eight standard 32-bit pixel formats are
supported. Limits are 128 resources, 4096×4096 dimensions, 256 MiB resource
storage and 16,384 backing entries. Invalid protocol requests receive errors;
invalid DMA and rings fail safely. There is no 3D acceleration or automatic
host-window resolution negotiation.

The signed Windows DOD driver reads EDID without negotiating its optional bit;
the device accepts this read-only query because EDID is offered. Selecting GPU
omits the separate RAMFB firmware device so VirtioGpuDxe supplies the sole boot
GOP and Windows can use the GPU for its main display. A final frame is retained
when Windows disables its scanout during clean shutdown.

```sh
make test-virtio-gpu
./tools/build.sh virtio-gpu-sanitized-tests
./build/virtio-gpu-sanitized-tests
python3 tools/smoke_windows_guest_tools.py
```

The installed-image runner makes a private child of an existing Windows disk,
uses non-formatting WinPE driver injection, and runs original ARM64 serial and
GDI probes after logon. Its temporary autologon credential is removed at probe
startup. It checks exact serial bytes, a 64 MiB balloon, and red/green/blue pixels
from the active GPU scanout, then requests normal Windows shutdown. Successful
bootstrap media and credential staging are deleted. Reports and the private
validated overlay remain under ignored `build/windows-tools-*`.

On 2026-10-26 all three probes passed: serial received 16 and transmitted 45
bytes; balloon inflated and returned 16,384 pages; GPU displayed 1280×720 with
zero command errors and verified RGB samples. CompactOS was confirmed. A
subsequent ordinary cold boot reached the lock screen on the GPU and shut down
through the Hyper-V guest service. [Native evidence](measurements/windows-virtio-gpu-2026-10-26.json).

`run11.sh` enables these devices with the pinned balloon PFN64 compatibility
option and a zero balloon target. Its active disk is
`media/windows11-arm64/system-tools-live.qcow2`, safely rebased onto the existing
immutable `system.qcow2` and checked byte-for-byte against the stopped validated
chain. The previous live disk remains available for recovery. This supplies
working device drivers; QEMU guest-agent, SPICE and balloon statistics user
services have not been installed by these probes.
