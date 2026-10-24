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
- `src/main.cpp`: current application/session orchestration, guest control and
  framebuffer production. It no longer includes or calls SDL.

The machine, CPU, JIT, disk tools and core tests do not include or link a GUI
library. Only the SDL adapter and its test need SDL compile/link flags. Package
flags use pkg-config rather than a hard-coded macOS Homebrew path.

Machine presets and execution selection are now exposed through
`--machine` and `--backend`; see [machine profiles](machine-profiles.md).
The presets currently share one concrete legacy platform. Planned modern
platforms reject launch until implemented.

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

The machine and execution layers are still concretely coupled in today's PC
implementation. Introduce a session adapter when a second machine/backend is
actually implemented, rather than declaring empty generalized interfaces now.
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
