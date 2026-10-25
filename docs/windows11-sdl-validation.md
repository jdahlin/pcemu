# Windows 11 SDL input and startup validation

Start the local installation with `./run11.sh` (4 GiB, two CPUs, user networking,
read-only qcow2 base and writable local `.pcd` overlay). Click to capture the
mouse; Cmd+G releases it. Shut down in Windows before closing when possible.

## USB corrections

Windows evaluates the slot context with only Max Exit Latency and Interrupter
Target populated. The previous controller tried to re-read the root port from
that partial context, returned Parameter Error, and Windows disabled the slot.
Both devices repeatedly failed immediately after configuration-descriptor reads.
Evaluate Context now updates only its defined fields, preserving the port,
device address, slot state, endpoint state and transfer dequeue pointers.
Endpoint-zero evaluation updates Max Packet Size without replacing its ring.
Configure Endpoint likewise preserves the root port, device address and EP0,
consuming only Context Entries and the newly added endpoint contexts. Its input
slot context does not repopulate the root port either. The regression fixture
now explicitly zeroes that field; preserving it in the old fixture hid this
second failure. Snapshot diagnostics count host key/pointer events separately
from delivered reports and show each slot's configuration and pending IN state.

Windows' inline OUT control data is also supported. The Immediate Data (IDT)
bit means a short report is stored in the TRB itself, not at the physical address
represented by those bytes. Treating a zero LED report as DMA to address zero
halted the controller after HID configuration. Regression coverage now includes
this request, and zero-byte transfers no longer attempt DMA or form invalid
host pointers. DMA failures report the failing guest address and length.

Short descriptor responses also retain their Short Packet completion code at
an Event Data TRB. The following status stage starts with a fresh byte count and
completion status. These rules were checked against the
[Intel xHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf)
and [QEMU's controller implementation](https://github.com/qemu/qemu/blob/master/hw/usb/hcd-xhci.c).

Regression tests reproduce the partial Windows context and short descriptor /
Event Data sequence, then exercise Enter press/release, capture clicks, relative
motion, buttons and focus loss through the shared frontend input adapter.
Controller tests pass with AddressSanitizer and UndefinedBehaviorSanitizer.
Shared input tests, 36 PCI/MMIO/framebuffer checks, and 13 native HVF CLI checks
also pass.

The first native correction reached the lock screen with 14 xHCI commands and
66 transfer TRBs, but user testing still found no input. This exposed the
Configure Endpoint bug above. Those counters were not proof of functional HID
input. Previously descriptor enumeration retried through 48 commands and 120
transfer TRBs. Native acceptance for the complete correction is recorded below.

## Startup profiling

The writable `.pcd` previously rebuilt its block map by scanning nearly 7 GiB
before firmware could run. Persistent disposable indexes now avoid that scan
on clean reopen: measured offline open/close changed from 7.906 to 0.164 seconds.
See [storage behavior and invalidation](storage.md#persistent-writable-overlay-index).

A five-second native startup sample found 2,637 of 4,075 main-thread samples
waiting for a Metal drawable. That renderer shares the VM thread, so compositor
waits also stall guest execution. macOS now uses SDL's software renderer; a
subsequent three-second sample put 1,939 of 2,434 main-thread samples in VM
execution instead. These are sampled stacks, not a measured total-boot speedup.
Windows still took roughly three minutes to reach the lock screen in the latest
run; its remaining boot delay is unresolved.

Local diagnostic outputs are ignored under `build/windows11-usb-debug/`,
`build/windows11-usb-software/`, and `build/windows11-usb-evaluate-fixed/`.
Temporary protocol tracing and input-injection hooks were removed from source.
