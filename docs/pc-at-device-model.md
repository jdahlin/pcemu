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

## First extraction (2026-10-24)

ATA and NE2000 now register named port regions in `Machine::bindDevices()`.
`IoPortSpace` rejects empty, out-of-range and overlapping registrations, forwards
the original port/value/width, and reports misses to the existing machine fallback.
It intentionally leaves width/alignment policy with each device. Other device
ports, CPU/JIT callbacks, snapshot encoding and CLI/platform names are unchanged.

Validation passed before and after extraction: the full `make test` suite,
Windows 3.11 Notepad file verification, and Windows 95 cold boot plus restart.
After extraction, machine-profile tests and all three DOS reset paths also passed.
The existing Win95 Calculator snapshot produced the expected pixels on three
independent JIT restores and one interpreter restore. Those task timings are
validation evidence only; no before/after performance conclusion is claimed.

Local evidence: `build/pc-at-baseline.log`, `build/pc-at-after.log`,
`build/pc-at-win311-{before,after}.log`, `build/pc-at-win95-{before,after}.log`,
`build/pc-at-reset-after.log`, `build/pc-at-profiles-after.log`, and
`build/pc-at-calculator-{verified,interpreter}/result.json`.
The calculator used a fixed executable copy at `build/pc-at-validation` because
concurrent workspace builds can replace `build/pcemu` during a measurement.

## Complete port registration (2026-10-24)

`src/pc_at_io.cpp` now owns the PC/AT port composition in `bindIoPorts()`:
PIC, PIT, PCI configuration, VGA/DISPI, PS/2, CMOS, speaker, system control and
debug console join ATA/NE2000 as named regions. `Machine::in/out` only dispatch
transactions and implement unmapped-port fallback. The small PS/2-read,
CMOS-read and PIC-write adapters remain machine methods until their respective
device state is extracted; this step does not claim that state ownership moved.

The composition deliberately preserves asymmetric transaction rules:

- PIC, VGA, PS/2, CMOS, speaker, system control and debug writes split into byte
  transactions, re-entering the bus for each byte, including adjacent gaps.
- PIT, PCI, DISPI, ATA and NE2000 receive the original width in one call.
- Reads remain whole transactions. PIT control reads use the old saved-port
  fallback. Retrace counting excludes PIC, PIT data, ATA and NE2000 reads.
- Port shadows, PIC mirror fields, keyboard deadlines and speaker gate ordering
  retain their previous behavior. Snapshot state and rebinding remain unchanged.

Validation: `make test` passed, including 106 input/video integration checks and
2,887 snapshot checks. Machine-profile tests, Win3.11 Notepad file verification,
Win95 cold boot/restart, and all three DOS reset paths passed headless.
Five independent Calculator snapshot restores passed before and after this step;
one interpreter restore also passed. Sequential runs after the other guest tests
finished had task-response medians of about 69 ms on both binaries. This bounded
end-to-end task is not a general CPU or I/O throughput benchmark, and concurrent
JIT work in the shared workspace prevents attributing timing differences solely
to this extraction.

Evidence: `build/pc-at-ports-tests.log`, `build/pc-at-ports-{win311,win95,reset,profiles}.log`,
`build/pc-at-ports-idle-{before,after}/result.json` and
`build/pc-at-ports-interpreter/result.json`. Fixed executable copies are
`build/pc-at-ports-before` and `build/pc-at-ports-after`.

## Device state and application responsibilities (2026-10-24)

`CmosRtc` now owns CMOS selection, hardware configuration and the adjusted VM
clock. `bindDevices()` supplies RAM/disk configuration after construction and
snapshot restore; reset clears selection while preserving time. The snapshot
codec writes the selector and time in their original field positions, preserving
the existing encoding. The injectable clock permits deterministic tests.

PC firmware/boot, BIOS services, BIOS text operations and PS/2 protocol methods
are separated into `pc_firmware.cpp`, `pc_bios.cpp`, `pc_bios_video.cpp` and
`pc_ps2.cpp`. `Machine` retains board composition, reset and interrupt wiring;
PS/2 and BIOS state still live there pending a separate ownership migration.

The application now uses `PcDisplay` for font/geometry/rendering/captures,
`session_diagnostics.*` for CPU/state output, and `cpu_benchmarks.*` for synthetic
workloads. These components are toolkit independent. The existing control-file
protocol, CLI, JSON schema and snapshot preview behavior are retained. Captures
omit the blinking cursor, matching the previous behavior; presentation tests
exercise cursor pixels headless without opening a window.

The shared responsibility model is documented in [modularity](modularity.md).
The subsequent PS/2 ownership migration is described below; CLI/control and
tracing responsibilities remain in the application loop.

Validation passed: the full `make test` suite (including 20 new deterministic
CMOS/RTC/presentation checks), machine-profile tests, Win3.11 Notepad file save,
Win95 cold boot/restart, and all three DOS reset paths. The existing Calculator
snapshot produced the expected pixels on three independent JIT restores and one
interpreter restore. Register/memory benchmark entry points ran with both engines
and matching final EAX/checksums. `make test-hvf` and the real Debian installer
language-selection boot also passed; the installed server was not changed.

Evidence: `build/pc-components-final-tests.log`, `build/pc-components-{win311,win95,reset,profiles}.log`,
`build/pc-components-calculator{,-interpreter}/result.json`,
`build/pc-components-benchmarks.log`, `build/pc-components-arm-tests.log` and
`build/pc-components-debian.log`. Snapshot tests retain the existing field-by-field
round-trip checks; the state JSON continues to be parsed by the profile and guest
workload checks. No GUI guest run was needed.

## Independent PS/2 controller (2026-10-24)

`I8042Device` (`i8042.*`) replaces the `Machine` PS/2 protocol methods
and scattered hardware fields. It owns the output buffer, command and parameter
state, keyboard replies/scans, byte deadlines, IRQ latches and attached mouse.
Its implementation only depends on the mouse model and standard C++ types.

| Interface | Responsibility |
|---|---|
| `poll(now, IrqState)` | Advance device time and return new keyboard/mouse IRQ edges |
| `readStatus` / `readData` | Poll, read register state and consume output bytes with the existing deadline |
| `command(value, a20)` / `write(value)` | Process controller/keyboard commands; return A20/reset effects |
| `enqueueScan` / `motion` | Accept host input without exposing internal queues |
| `reset()` | Restore hardware defaults independently of BIOS state |
| `state()` | Read-only diagnostic/checkpoint view |

The board passes the existing PIC mask/in-service conditions as values; that
preserves current scan backpressure without coupling the device to a PIC class.
The device reports edges rather than accessing interrupt controllers. A20 reads
sample the board's current A20 state, and writes return an optional value so
"no change" remains distinct from disabling A20. No runtime board callbacks or
references are embedded in checkpoint state.

`pc_input.*` handles automation text-to-scan translation. `HostKeyboard` uses a
scan sink bound to the current controller. `Machine::key` only injects input and
wakes the CPU. BIOS key queues, scan-to-BIOS translation and mouse callback packet
assembly remain firmware responsibilities. The attached mouse endpoint retains
its own existing command/status API for BIOS mouse services.

Snapshot codecs keep all hardware fields in their existing versioned positions,
including the version-2 mouse/auxiliary fields. Only the codec and test fixture
helper have mutable access to controller checkpoint state. Restores continue to
rebind the board's port callbacks; controller state itself has no pointers to the
old or staged machine.

Validation passed headless: 28 standalone controller checks linked only with the
mouse/input components (no `Machine`, CPU, JIT or PIC), the full `make test` suite,
2,889 snapshot/COW checks, profile checks, Win3.11 Notepad file verification,
Win95 cold boot/restart and all three DOS reset paths. Three JIT restores and one
interpreter restore of the existing Calculator checkpoint produced the expected
pixels. Snapshot regression checks additionally consume the restored output byte
and resume queued replies at the preserved serial deadline.

Evidence: `build/ps2-standalone-tests.log`, `build/ps2-controller-final-tests.log`,
`build/ps2-{win311,win95,reset,profiles}.log`, and
`build/ps2-calculator{,-interpreter}/result.json`.

## QEMU references

- [Memory and I/O regions](https://www.qemu.org/docs/master/devel/memory.html)
- [Device lifecycle](https://www.qemu.org/docs/master/devel/qdev-api.html)
- [Device and host resource separation](https://github.com/qemu/qemu/blob/master/docs/qdev-device-use.txt)
