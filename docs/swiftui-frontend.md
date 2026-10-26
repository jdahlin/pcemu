# Native macOS library and embeddable sessions

Build the Apple Silicon SwiftUI app with `make app-native`, then open
`build/PCEmu Library.app`. This is a local checkout app and uses the checkout's
firmware, dynamically linked Homebrew zstd, and existing guest images.

The sidebar supports VM creation, settings, double-click to open, context menus,
Finder reveal, and removal from the library. The x86 guest display is embedded in
the window. Toolbar and VM menu actions pause/resume, reboot, and suspend to a
new snapshot. Quitting the app suspends an active x86 VM before termination;
snapshot failure leaves the session alive and preserves the selected checkpoint.
Reboot resets guest hardware and loses unsaved guest work.

Click the display to focus the keyboard and capture the relative mouse. Press
Control–Option to release both; changing focus also releases held keys/buttons.
Right-click captured guest input goes to the guest. The uncaptured display and
VM rows expose host context menus. Absolute pointer integration and uploaded
native guest cursors are supported. Monochrome cursor inversion is approximated
as white because Cocoa cursors cannot invert pixels behind them.

## Default registrations

On launch, the app discovers Windows 3.11 and Windows 95 disks under `media/`,
then the existing recovered disks under `/Volumes/data/pcemu/recovered/`.
Windows 3.11 cold boot enters `WIN /S`; Windows 95 initializes its VGA ROM.
Windows 11 uses `media/windows11-arm64/system.qcow2` and the existing `run11.sh`
profile, preserving its live overlay and UEFI variable files.

The current registered disks are:

- Windows 3.11: `/Volumes/data/pcemu/recovered/win311.qcow2`
- Windows 95: `/Volumes/data/pcemu/recovered/win95.qcow2`
- Windows 11: `media/windows11-arm64/system.qcow2`

Defaults merge into existing libraries without replacing user settings or
re-adding entries deliberately removed. Disks that are absent are discovered on
a later launch once their volume is mounted. Registration reads paths only; it
does not copy, boot, or modify guest disks. `make register-vms` updates the local
library without opening a GUI.

The library and default-registration ledger live in
`~/Library/Application Support/PCEmu/`. Suspend writes unique checkpoints beneath
its `Snapshots/` directory; earlier checkpoints remain available. Removing a VM
only removes its library record. Keep snapshot backing chains available.

Restore is the default whenever a VM has a selected checkpoint. A VM without a
checkpoint cold boots its configured disk. Cold boot writes to that disk; restore
uses the engine's private writable clone semantics. Memory changes take effect on
cold boot because a snapshot restores its original RAM size.

## Engine support

The embedded x86 session supports 2–64 MiB RAM and one vCPU. The UI exposes that
actual CPU limit. One x86 VM may run per process; process-global capture signals
and JIT diagnostics have not been made independent per session.

Windows 11 currently opens in the existing ARM64 SDL window, or activates that
window if it is already running. Its settings support 256–16384 MiB RAM and
1–8 vCPUs; the launcher generates matching firmware tables. Input and power
controls remain in the guest window. ARM64 saved-state restore, library pause and
reboot, and an embedded SwiftUI ARM64 display are not implemented. Closing the
library does not terminate that separate guest window.

## Library boundary

`pcemu-session` is a static C++ library with no SDL frontend implementation or SDL
runtime symbols. See `src/pc_session.hpp`:

```cpp
pc::PcSessionOptions options;
options.machine = "win95";
options.snapshot = "/path/to/checkpoint";
options.rom = "/path/to/vgabios.bin";
// factory creates a pc::Frontend on this session's worker thread.
int status = pc::runPcSession(options, factory, controls);
```

The frontend owns input polling and presentation through `pc::Frontend`.
`pc::SessionControl::takeCommands()` supplies in-memory lifecycle commands;
errors are delivered through `reportError`. Supported commands include `pause`,
`resume`, `reboot`, `save-snapshot PATH`, `load-snapshot PATH`, `suspend PATH`,
`close` (guest shutdown), and `quit` (immediate stop). `suspend` exits only after a
successful save. Call the blocking runner on a worker and bridge toolkit work to
its UI thread. Sequential sessions are supported. A null frontend factory is
valid for headless sessions.

The CLI is a thin consumer of the same runner. Configuration parsing happens at
session startup. Frontend and command calls occur between execution batches;
the CPU/JIT dispatch path gains no callbacks, locks, or per-instruction indirection.
SwiftUI copies the latest framebuffer through a mutex-protected C bridge and
samples it at 30 Hz; those presentation costs are distinct from library
extraction. Headless extraction measurements are recorded in
`measurements/swiftui-session-extraction-2026-10-26.json`.

## Validation

`make test-native` builds the app, runs the headless typed-library test and CLI
pause/reboot/suspend/restore test, and validates default discovery, migration,
idempotence, and deliberate removal with temporary placeholder files. No
installed OS, GUI guest run, or large VM fixture is required. Existing frontend
input and snapshot/COW tests cover the shared policies and storage behavior.
