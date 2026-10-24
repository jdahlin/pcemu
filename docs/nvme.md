# NVMe storage on ARM64

The original NVMe PCI controller boots the unmodified Microsoft ARM64 Validation
OS 26100.9549 VHDX to its graphical administrator `cmd.exe` prompt. Windows uses
its inbox `stornvme.sys`; no VirtIO driver injection or RAW conversion is needed.
VirtIO remains the default for existing guests.

```sh
make build/pcemu-hvf firmware-arm64
python3 tools/observe_validation_os.py --disk-controller nvme
```

Or attach a private disk explicitly:

```sh
build/pcemu-hvf --machine arm64-uefi --disk-controller nvme \
  --disk media/validation-os-arm64-vhdx/ValidationOS.vhdx \
  --vars build/windows-nvme/vars.bin --timeout-ms 60000 \
  --cpu-state --framebuffer-ppm build/windows-nvme/console.ppm
```

VHDX writes are direct unless `--disk-overlay` is provided. The existing disk
backend supplies VHDX validation, locking, allocation and persistence. The
NVMe controller uses the same single PCI function/BAR and GIC SPI 48/INTA route
as the selectable virtio endpoint. No QEMU device implementation is linked.
The CLI's existing `virtioRequests` field counts either controller's commands;
`--cpu-state` adds NVMe admin/I/O/error/reset counters and last error/status.

## Implemented subset

- NVMe 1.1 register interface, BAR sizing, enable/reset/shutdown, interrupt
  mask set/clear, and legacy INTx with PCI interrupt-disable support.
- One 512-byte-sector namespace backed by `Disk`, up to 15 I/O queue pairs,
  physically contiguous queues of up to 256 entries, 64-byte submissions and
  16-byte completions, phase wrapping and completion backpressure.
- Identify controller/namespace/namespace-list, queue create/delete, number of
  queues and selected initialization features, basic health/firmware/error-log
  responses. Up to four asynchronous event requests remain pending until reset;
  there are no generated asynchronous notifications.
- Read/write/flush, 4 KiB PRPs including one PRP list, up to 128 KiB per transfer.
  Every DMA segment must be in guest RAM and the entire scatter list is checked
  before copying. No volatile write cache is advertised; writes are flushed.
  Unsupported commands and invalid transfers return errors; invalid queue setup
  fails ready/fatal status checks.

MSI/MSI-X, SGLs, other page sizes, metadata, discard, write zeroes, namespace
management, multiple controllers, interrupt coalescing delays, external debug,
background I/O and exhaustive NVMe conformance are not implemented. Feature
coalescing values can be saved/read but completions are delivered immediately.
Abort currently reports command-not-found; asynchronous event cancellation
beyond controller reset is not supported. There is no live graphical/input
frontend in this runner. Reaching `cmd.exe` confirms this boot milestone; user
command execution, clean Windows shutdown/restart and long-term disk integrity
still need separate end-to-end checks.

## Verification

`make test-nvme` checks identify data, queue setup/deletion, queue negotiation,
read/write round trips, PRP scatter lists and invalid DMA, namespace/LBA bounds,
interrupt masks, PCI interrupt disable, completion phase wrap/backpressure,
reset and shutdown. It also runs in `make test-hvf`. The suite passes ASan/UBSan.

`make smoke-nvme` boots EDK II on an original synthetic VHDX, writes a FAT file
through NVMe, then recreates the VM and verifies its contents. No proprietary
media is needed and no COW file is created.

The original Windows disk was observed for 60 seconds in
`build/validation-os-boot-qsn2t9ig`: the captured GOP image contains
`Microsoft Windows [Version 10.0.26100.9549]` and `C:\Windows\System32>`.
The same disk on virtio reached `INACCESSIBLE_BOOT_DEVICE (0x7B)`, matching the
lack of an installed VirtIO storage driver in this particular image.
See [media preparation and boot history](windows-arm64-media.md).

Interface references: [NVM Express specifications](https://nvmexpress.org/specifications/)
and [EDK II's NVMe register/command definitions](https://github.com/tianocore/edk2/blob/master/MdePkg/Include/IndustryStandard/Nvme.h).
