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
