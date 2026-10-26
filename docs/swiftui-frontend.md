# Native macOS library and embeddable sessions

Build the Apple Silicon SwiftUI app with `make app-native`, then open
`build/PCEmu Library.app`. This is a local checkout app and uses the checkout's
firmware, dynamically linked Homebrew zstd, and existing guest images.

The sidebar supports VM creation, settings, double-click to open, context menus,
Finder reveal, and removal from the library. All three guest displays are embedded in
the window. Toolbar and VM menu actions pause/resume, reboot, and suspend to a
new snapshot. Quitting the app suspends the active VM before termination;
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

Windows 11 now runs inside the same SwiftUI display as the x86 machines; its
legacy registration field remains readable for existing libraries. `run11.sh`
continues to provide a separate SDL/SPICE launch when explicitly requested.

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

Windows 11 uses the embedded Hypervisor.framework ARM64 session, with the same
pause/resume, reboot, suspend-on-quit, input capture, and restore-on-open actions.
Its settings support 256–16384 MiB RAM and 1–8 vCPUs; cold boot generates matching
firmware tables. A checkpoint restores its saved topology. Shut down an existing
SDL/SPICE VM before opening the same writable disk in SwiftUI; the disk and UEFI
variable locks prevent concurrent use.

ARM checkpoints are currently schema-versioned snapshots. They include native
VP/SIMD/system-register and GIC state, pending SMP exits, RAM, firmware assets,
UEFI flash, VirtIO transports/device queues, GPU resources/cursor, USB transfers,
Hyper-V/SynIC/VMBus services/rings, and HDA state. Pause freezes the architectural
virtual counter and Hyper-V reference time so guest timers retain their remaining
time. Host network sockets reconnect after restore; existing connections may
close. See [ARM checkpoint design and validation](arm-snapshots.md).

The current ARM checkpoint profile supports the primary VirtIO disk. NVMe,
extra disks, SPICE agent connections, and external guest-agent sockets are
rejected explicitly. Disk checkpoints use same-volume APFS clones to freeze
contents without copying large images; cross-volume/non-clone storage fails
with an actionable error. The immutable backing chain must remain available.

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
`close` (guest shutdown), and `quit` (immediate stop). The ARM runner supports those lifecycle commands
except in-place `load-snapshot`; restore uses a fresh native VM before any VP
runs, as required by Hypervisor.framework. `suspend` exits only after a successful
save. Call the blocking runner on a worker and bridge toolkit work to
its UI thread. Sequential sessions are supported. A null frontend factory is
valid for headless sessions.

`pcemu-arm-session` exposes `runArmSession(ArmSessionOptions, factory, controls)`
with the same frontend and command boundary. It contains no SDL display or audio
backend: audio playback is an optional host callback. The SwiftUI bridge supplies
native presentation/input plus SDL audio without creating an SDL window.

The x86 CLI is a thin consumer of the same runner. Configuration parsing happens at
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

`make test-arm-native` runs archive-bound checks, native SMP/GIC/register/clock
restore, and a 256 MiB synthetic full-machine/lifecycle checkpoint test headless.
For the installed Windows image, prepare its matching platform and run
`./build/arm-snapshot-tests --windows`. That test uses the existing stopped
`system-tools-live.qcow2` image with a private compressed child; it never opens
the live Windows writable overlay. Successful tests remove checkpoint RAM/disk
files and private children, retaining only small diagnostics and screenshots.

The x86 runner keeps the machine, JIT tables and optional trace history on the
heap so embedded startup fits Dispatch's small worker stacks. The library
regression test runs cold boot, restore, lifecycle controls and trace capture
on a 512 KiB pthread, with a headless display sink for the embedded UI path.
