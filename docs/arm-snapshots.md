# ARM64 native sessions and checkpoints

The SwiftUI library embeds Windows 11 through `pcemu-arm-session`, independently
of the SDL/SPICE CLI. `runArmSession` owns creation/destruction on one dedicated
thread, calls a supplied `FrontendFactory`, and polls `SessionControl` between
native execution slices. A native bridge carries frames, cursor shapes, USB HID
input and lifecycle commands to the same SwiftUI view used by the x86 machines.
No frontend callbacks or serialization calls execute inside guest instructions.
The native command queue has an atomic empty check, taking its mutex only when
commands are pending. SDL is used for optional audio output in the application,
not for display/input or by the embeddable ARM session library.

## Quiescence and time

Every `ArmVirtMachine::run` returns with all VPs stopped. Pause stops calling it;
input is released and the architectural virtual counter and Hyper-V reference
counter freeze together. Resume adjusts each VP's timer offset, invalidates and
republishes the reference page, and restarts audio servicing from the current
host time. A restored VM begins with frozen counters and only advances when
resumed. Guest virtual-timer comparisons and synthetic deadlines retain the
same time base; host wall-clock synchronization can subsequently correct UTC.

VP APIs execute on their creating threads, including checkpoint operations on
secondary VPs. Cached register/PC writes and pending CPU_ON initialization are
settled before capture. The pending exit queue and selected VP are retained so
an already-reported secondary MMIO/HVC exit is not silently discarded.

Native GIC capture uses `hv_gic_state_create`, with distributor/redistributor
state exported as an opaque checked buffer. Restore creates a VM and every VP
without running the GIC bootstrap, restores the GIC, then restores each VP's
GIC CPU interface, general/SIMD/system registers and virtual timer mask/offset.
This follows Apple's requirement to restore the GIC before any VP executes:
[Hypervisor.framework GIC restore](https://developer.apple.com/documentation/hypervisor/hv_gic_set_state(_:_:)).
Unsupported native system registers are omitted at capture; restored read-only
CPU features must match. Active SME/ZA extended-vector state is rejected rather
than silently losing vector contents. The host framework can reject an opaque GIC checkpoint
following an incompatible OS change. Such failures require a cold boot and
leave the previous checkpoint intact.

## Explicit device state

`arm_snapshot_state.hpp` enumerates device fields. `ArmSnapshotAccess` is the
only serialization friend; there is no byte-copying of device objects, spans,
callbacks, mutexes, file handles, or sockets. A fresh constructor rebuilds host
bindings and memory views before restoring state.

The inventory covers flash command state, fw_cfg items/DMA cursor, UART, RTC,
OS lock state, VirtIO queues/feature negotiation/interrupts, PCI/MSI-X routing,
network queues, entropy, serial ports, balloon state, GPU resources/backing and
cursor, xHCI slots/endpoints/TRB and event rings/input queues, HDA DMA/playback
position, and Hyper-V services. GPADL descriptors and generations are restored
before VMBus ring views are rebound. The fixed channel inventory retains its
newly bound handlers. Presentation caches are invalidated after restore.
Host network connections are intentionally recreated; this resembles a link
interruption for existing guest connections.

The current snapshot profile rejects NVMe, secondary disks, external guest-agent
sockets and a SPICE agent connection. The embedded Windows 11 profile uses a
primary VirtIO disk, USB input, VirtIO GPU/serial/balloon/network and HDA.

## Storage and validation

A checkpoint freezes the VM, flushes disks/variables, clones the writable disk
on APFS, serializes device/CPU metadata, and streams RAM in checksummed 4 MiB
zstd chunks. It publishes a new directory atomically only after all writes
succeed. Existing destinations are rejected. Failures remove the staged
checkpoint, resume a previously running VM, and report the error to SwiftUI.
No raw multi-gigabyte RAM dump or full disk copy is created.

Disk clones must be on the same clone-capable volume; unsupported storage fails
instead of allocating a large fallback copy. Checkpoints retain their immutable
backing disk path and verify its size/modification identity before restoration.
The backing chain must remain available and unchanged. Required small firmware
and ACPI assets and the variable store are included in the checkpoint directory.
Restore creates a private writable disk clone/variable file and removes the run
directory after VM destruction. A reboot opens that same private disk/variable
store with fresh CPU/device state, preserving guest disk writes.

The format is experimental and specific to the engine schema/ABI. A field-layout fingerprint
and schema version reject incompatible metadata before creating the VM; compressed
checksums, bounded collection lengths, exact RAM extents and trailing-byte checks
reject corrupt/truncated data. A fresh restore can fail without altering the saved
checkpoint or a running VM. Large all-zero RAM chunks do not commit fresh host
pages on restore. Ordinary rebuilds retain compatibility; schema changes currently have no migration path.

`make test-arm-native` validates the archive, SMP/native GIC and architectural
clock restore, and the complete device/RAM/disk lifecycle with 256 MiB RAM-only
synthetic firmware. `make test-native` retains the x86/frontend regressions.
The optional headless Windows test reuses the stopped guest-tools image with a
small compressed child, compares the restored 1280×720 framebuffer exactly,
resumes integration services, sends USB Enter and exercises the shared reboot
runner. Successful runs remove RAM checkpoints and temporary disks.

The 2026-10-26 headless Windows run passed at 1280×720 with two VPs and 4 GiB
RAM: the restored framebuffer was byte-identical, heartbeat responses advanced
from 1 to 8 with no VMBus channel faults, USB Enter visibly opened the password
prompt, and the shared reboot runner returned to the lock screen. Temporary
checkpoints and guest disks were removed. The [validation record](measurements/windows11-native-snapshot-2026-10-26.json)
contains the screenshot hashes and small retained evidence paths. Native backend
regressions also pass: 304 self-test checks, 60 GIC checks, and 2,503 counter checks.
