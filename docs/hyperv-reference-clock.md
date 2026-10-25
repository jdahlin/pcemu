# ARM64 reference-clock page implementation

The portable `ArmHyperVReferenceClock` model implements the shared reference
page layout and fixed-point conversion from the
[Microsoft TLFS timer contract](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers).
It accepts a paired virtual-counter/reference-time sample and counter frequency,
computes a 64-bit scale and signed offset with 128-bit intermediates, and
publishes a nonzero sequence after the mapping. Sequence zero invalidates the
page. All guest VPs must be stopped during model operations.

Registration validates reserved bits and the full RAM page. Invalid configuration
leaves existing state and RAM untouched. Replacement/disable invalidate the old
page. Unrepresentable updates invalidate the current mapping for guest fallback;
a later representable sample can republish it. Generation increments skip zero.
Ratios requiring a multiplier of one or greater are unrepresentable and rejected.

`make test-arm-hyperv-reference-clock` passes 4,146 checks, including reserved
page bytes, guard memory, rate/rounding boundaries, signed offsets, invalid
registration, replacement, rebasing and fallback recovery. The same checks pass
under AddressSanitizer/UBSan.

This is the page model, not advertised guest support. Native integration still
requires a proven guest virtual-counter/frequency sample, common VP offsets,
registration dispatch and feature discovery, and updates synchronized with HVF
host-sleep offset changes. Native guest reads must be compared with hypercall
reference time before actual Windows consumption or performance is claimed.
The existing HVF backend changes virtual-counter offsets after host sleep, so
using a one-time host timestamp without this synchronization would be incorrect.

## Native counter sampling prerequisite

`HvfMachine::virtualCounterSample()` now returns the virtual count, native
frequency and applied sleep correction while all VPs are stopped. It samples
`mach_absolute_time` minus the actual primary HVF timer offset, which is shared
with secondary VPs. It does not itself change offsets or enable the reference
page privilege.

`make test-arm-hvf-reference-counter` executes actual CNTVCT/CNTFRQ reads on
two VPs and compares them against host-side samples. Injecting five seconds
of host sleep and allowing the backend's next clock synchronization verifies
that both VPs and subsequent host samples include the same adjustment. All
29 native checks pass. This establishes the counter sampling prerequisite;
shared-page guest reads and atomic rebasing before VP resumption remain to be
implemented and tested.

## Native shared-page rebasing

The backend now offers `setCounterRebaseHandler`: with all VPs stopped, it
notifies a consumer immediately before changing virtual-counter offsets and
after every VP has received the new offset. A failed update leaves the page
invalidated and aborts the run; no guest executes with a partially updated
mapping. Consumers must remove borrowed callbacks before their lifetime ends.

`make test-arm-hvf-reference-page` executes sequence/scale/offset loads,
CNTVCT, UMULH and signed-offset addition in real guest instructions on both
VPs. An injected five-second counter adjustment invalidates then republishes
the model page. Guest-computed reference values stay close to the independent
monotonic domain instead of jumping five seconds, and both VPs observe the
new publication sequence. The initial native run passes 48 checks; counts
can vary when a VP completes additional loops before its peer.

This verifies backend/page cooperation, not Windows use: synthetic register
dispatch and feature discovery remain disconnected.
