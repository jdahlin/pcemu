# PC/AT device model refactoring

The first target is the existing x86 PC used by Windows 3.11 and Windows 95.
`PcAtMachine` is the working architectural name; the existing C++ `Machine`,
CLI profiles and `pc-legacy-v1` platform identifier remain unchanged during the
initial extraction. Future historical machines should compose reusable devices,
not become branches in one universal legacy machine.

## Boundaries

- Machine composition owns devices, port mappings, interrupt wiring and firmware.
- Devices own register semantics and mutable hardware state.
- `IoPortSpace` routes an access to a registered device without knowing its type.
- Host disk, network and presentation services remain separate from guest hardware.
- Interpreter and JIT keep the existing CPU input/output callbacks. RAM and VGA
  memory fast paths are outside the port router.

Use small C++ components and composition, inspired by QEMU's MemoryRegion and
device/bus separation. A dynamic object/type system is unnecessary here.

## Compatibility inventory

| Hardware | Current port map | Wiring / behavior to preserve |
|---|---|---|
| Master/slave PIC | 20–21, a0–a1 | Cascade IRQ2; existing acknowledge/EOI behavior |
| PIT and speaker gate | 40–43, 61 | Time is CPU instruction count divided by four |
| PS/2 controller | 60, 64 | Keyboard IRQ1, mouse IRQ12; queued byte deadlines |
| CMOS/RTC | 70–71 | Host clock plus saved offset; existing BCD register behavior |
| System control | 92 | A20 and reset request |
| ATA primary | 1f0–1f7, 3f6 | IRQ14; status read at 1f7 clears the machine IRQ latch |
| VGA/DISPI | 3b0–3df, 1ce–1cf | Existing retrace behavior and framebuffer fast paths |
| PCI configuration | cf8–cff | Mechanism 1; existing display function identity/BARs |
| NE2000 | 300–31f | IRQ10; existing remote DMA byte/word/dword behavior |
| Debug output | e9 | Host character output |

Addresses are hexadecimal. Firmware/BIOS services, software floppy services and
unmapped-port behavior remain as implemented. Most fallback reads return the
saved port byte; fallback wide writes split into consecutive byte writes. Device
accesses that already accept a width must remain single transactions at the
starting port, even at a region boundary (ATA data is not adjacent registers).
Do not introduce stricter alignment/width rejection during extraction.

The router is runtime wiring, not snapshot state. Rebuild it in `bindDevices()`
after snapshot staging moves the machine, just like CPU callbacks. Preserve
existing snapshot encoding and reset order.

## Commit-sized implementation sequence

1. Record the existing test baseline and this compatibility contract.
2. Add the port router and migrate only ATA/NE2000 registration. Test overlap
   rejection, boundaries, transaction widths, IRQ side effects and rebinding.
3. Migrate remaining port mappings, preserving read/write asymmetries and fallback
   precedence. Extract CMOS/RTC and PS/2 state into devices in separate changes.
4. Make interrupt, instruction-clock and reset connections explicit, retaining
   their current delivery order. Avoid unrelated timing changes.
5. Separate PCI bus/configuration from the display function, then consolidate
   device snapshot ownership without silently changing the on-disk format.

ARM, virtio, NVMe, new machine hardware and execution-engine changes are outside
the first implementation. Do not rename CLI/platform identifiers as a side effect.

## Validation gates

Run x86 checks headless. Use private disks/overlays and leave installed sources
unchanged. Evidence and media remain ignored under `build/` and `media/`.

- Before and after the first routing change: `make test`.
- Windows 3.11: `python3 tools/smoke_win311.py`, including Notepad save and exact
  FAT file-content verification. This currently uses the default x86 profile,
  16 MiB RAM and default JIT path.
- Windows 95: `python3 tools/smoke_win95.py --restart`, including cold boot,
  verified desktop and guest-requested restart. The fixture explicitly uses
  64 MiB RAM, although the `win95` profile defaults to 32 MiB.
- After routing changes: machine-profile tests and reset smoke test; existing
  snapshot tests must exercise restore/rebinding.
- Before broader extraction: repeat the same framebuffer-verified snapshot task
  with `tools/bench_snapshot_task.py`, recording workload, snapshot, engine mode,
  binary identity and repeated timings. Compare interpreter/JIT on bounded tasks
  when diagnosing correctness. Do not compare different JIT modes as refactor cost.

The baseline includes any pre-existing workspace changes; it is not automatically
equivalent to HEAD. Record failures before editing and do not count them as passes.

## QEMU references

- [Memory and I/O regions](https://www.qemu.org/docs/master/devel/memory.html)
- [Device lifecycle](https://www.qemu.org/docs/master/devel/qdev-api.html)
- [Device and host resource separation](https://github.com/qemu/qemu/blob/master/docs/qdev-device-use.txt)
