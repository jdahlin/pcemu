# Frontend and machine boundaries

SDL is the only graphical frontend for now. GTK and SwiftUI are possible future
adapters, not current deliverables. Avoid adding more frontends until needed.

The current separation is deliberately small:

- `src/frontend.hpp`: toolkit-independent input events and borrowed ARGB frames.
- `src/frontend_sdl.cpp`: SDL window, renderer, texture, event translation and
  pointer capture. Its lifetime owns SDL initialization and cleanup.
- `src/host_key.hpp`: USB HID Keyboard/Keypad usage identifiers shared by the
  application boundary. These are physical keys, not characters or PC scan codes.
- `src/host_keyboard.cpp`: the current PC machine's HID-to-set-1 translation,
  including extended keys, delayed host Command handling and focus release.
- `src/main.cpp`: application/session orchestration and guest control. It no
  longer owns pixel rendering, diagnostic serialization or synthetic benchmarks.
- `src/pc_display.*`: PC font lookup, BIOS display geometry, toolkit-independent
  pixels, blinking cursor and PPM captures.
- `src/session_diagnostics.*`: CPU diagnostic output and the existing state JSON
  schema, using an explicit capture context supplied by the session.
- `src/cpu_benchmarks.*`: register and memory benchmark workloads, independent
  of CLI parsing and the frontend.

The machine, CPU, JIT, disk tools and core tests do not include or link a GUI
library. Only the SDL adapter and its test need SDL compile/link flags. Package
flags use pkg-config rather than a hard-coded macOS Homebrew path.

Machine presets and execution selection are now exposed through
`--machine` and `--backend`; see [machine profiles](machine-profiles.md).
The presets currently share one concrete legacy platform. Planned modern
platforms reject launch until implemented.

## Device and firmware model

Keep the following responsibilities explicit for both PC/AT and ARM machines:

| Layer | Owns | Must not own |
|---|---|---|
| Device | Register behavior, mutable device state, reset semantics | CLI or frontend state |
| Bus/transport | Address decoding and forwarding complete transactions | Device command implementations |
| Machine composition | Device lifetimes, hardware configuration, IRQ wiring and reset order | Image rendering or diagnostic serialization |
| Firmware adapter | BIOS services, boot contract and firmware data structures | Host window/input implementation |
| Session/presentation | Execution orchestration, controls, captures and pixels | Hardware register protocols |

`machine.cpp` now contains PC/AT composition, reset, interrupt delivery and port
fallback. BIOS services are in `pc_bios.cpp`, firmware initialization/boot in
`pc_firmware.cpp`, BIOS text operations in `pc_bios_video.cpp`, and PS/2 protocol
methods in `pc_ps2.cpp`. These firmware/PS2 methods still operate on the machine;
file extraction does not claim their state is fully encapsulated yet.

`CmosRtc` (`cmos_rtc.*`) owns the PC's CMOS register selection and adjusted VM
clock. The machine configures installed RAM/disks, binds its ports and resets its
selector. BIOS and snapshots use the same device clock. Runtime configuration and
clock callbacks are rebuilt or supplied separately from serialized state.

For ARM, individual device models and the RAM framebuffer already own their
state; `arm_virt_devices.cpp` composes their MMIO/PCI connections. See
[PC/AT extraction](pc-at-device-model.md) and
[Debian device composition](debian-device-model.md).

Use composition and capability-specific interfaces. Do not introduce a universal
device base with unused MMIO/PIO/DMA/frontend methods. Existing reset order and
versioned snapshot codecs remain explicit until each device can safely own its
complete lifecycle. The next ownership boundary is the PC PS/2 controller;
CLI/control parsing and tracing also remain candidates for session extraction.

## Future structure

Keep four choices independent as more machines become useful:

| Boundary | Responsibility | Examples of future implementations |
|---|---|---|
| Frontend | Windows, input, presentation, user actions | SDL, GTK, SwiftUI |
| Machine model | Guest-visible devices, interrupts, firmware and boot contract | Existing x86 PC, ARM virtual platform |
| Execution backend | Execute the machine's supported CPU architecture | Interpreter/JIT, Apple Hypervisor, Linux KVM |
| Host services | Files, networking, executable memory and platform facilities | macOS, Linux |

These are compatibility-constrained choices: an ARM hardware backend does not
execute our x86 PC model. A guest OS is software installed on a suitable machine
model, not a separate frontend or execution backend.

The machine and execution layers remain concretely coupled in the legacy PC
implementation. The experimental `HvfMachine` is a separate ARM64 platform and
backend in `pcemu-hvf`, with no x86 CPU or GUI dependencies; see
[Hypervisor backend](hypervisor.md). It runs bare-metal code; the concrete `ArmVirtMachine` additionally boots EDK II
to a serial UEFI Shell, independently of the x86 device model.
Introduce the shared session adapter when integrating OS boot and frontend
control for that backend, rather than routing hypervisor operations through
the existing x86 CPU.
That adapter should expose start/pause/reset, bounded execution, input queues,
frames and checkpoint operations. Keep virtual dispatch out of per-instruction
and per-memory-access paths; choose a backend once and run batches within it.
Snapshots should retain machine/backend identity and architecture-specific CPU
state alongside shared disk storage.

A future GTK adapter can consume the frame/event boundary. A SwiftUI application
will also need a thin C or supported C++ interop bridge to a session, with VM work
off the main UI thread and explicit frame ownership. Neither adapter should
access x86 registers, PS/2 queues, PCI configuration or hypervisor handles.

## Validation

`build/frontend-tests` uses SDL's dummy driver to exercise normalized physical
keys, host shortcuts, pointer axes/buttons/wheel, focus loss, frame resize and
resource lifetime. Existing input/video integration tests continue to check the
PC translation. An independent Windows 95 Excel startup through the SDL frontend
is pixel-verified in `build/frontend-sdl-excel/`.
