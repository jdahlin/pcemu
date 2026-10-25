# Experimental ARM64 Hyper-V interface

`--hyperv basic` enables the initial ARM64 discovery/register interface.
The default is `off`. This independent TLFS subset does not provide VMBus,
SynIC, synthetic timers or nested virtualization. Address-space TLB-flush
hypercalls are supported as described below.

Prepare matching firmware assets:

```sh
python3 tools/build_arm_platform.py --cpus 1 --ram-mb 8192 --hyperv
```

Add `--ram-mb 8192 --hyperv basic --hyperv-stats build/hyperv.json` to the
ARM UEFI launch. The runner selects `cpus1-ram8192-hyperv` assets and rejects
an ACPI Hyper-V identity inconsistent with its runtime flag. Normal private
disk/variables requirements still apply.

## Implemented contract

SMCCC vendor UID discovery uses `0x8600ff01`, returning the Hyper-V interface
UUID. Version/feature queries describe supported calls only. The generated
ACPI FADT identifies `MsHyperV` only in the Hyper-V variant. PSCI retains CPU,
reset and shutdown handling.

SMCCC `HVC #0` with function `0x46000001` and legacy `HVC #1` support
GetVpRegisters (0x50) and SetVpRegisters (0x51), using memory or fast transport.
Validation covers repetitions, resume indices, partial completion, reserved
fields, VTL0, self-partition/current-VP restrictions, page bounds and overlapping
buffers. Guest identity is partition-wide, initially zero and writable on the
boot VP. Reading VP index requires an established identity.

The partition info page register (0x90015) starts disabled. The guest can
register an aligned 4 KiB page in its own RAM with bit 0 enabled. Reserved bits,
unmapped pages and nonzero upper 128-bit halves fail without changing the old
registration or memory. Successful registration clears the page; disabling it
does not overwrite RAM. This backend does not queue asynchronous address-space
flushes, so the pending-flush word remains zero. Other fields remain zero;
additional partition-info features are not advertised.

Per-VP TLBI control (0x90016) retains the guest's bit-0 opt-in and rejects
reserved bits. ARM architectural TLBI instructions continue through HVF;
there is no queued hypervisor flush or claimed flush acceleration. The
synthetic vector base (0x40400) is per-VP, 2 KiB aligned, and independent of
VBAR_EL1. The synthetic ESR (0x40401) reads zero because this subset has no
synthetic exception producers. Vector registration does not enable SynIC or
timer delivery. State is initialized afresh when the VM is recreated.

The early interface-version register (0x90006) returns `Hv#1`, as specified
by Microsoft's ARM64 guest header. Feature registers expose guest identity,
VP index, partition reference counter, register access and fast output.
No synthetic timer, SynIC, VMBus, flush or scheduling facilities are advertised.
The version is PCEmu interface 1.0 build 1, not a Microsoft release identity.
Unsupported operations/registers return errors rather than fabricated success.

Fast transport follows Microsoft's OpenVMM 16-byte register-pair layout:
SMCCC output follows input pairs; legacy output uses the final pairs ending
at X16. A single-register Get returns X6/X7 and X15/X16 respectively.

## Evidence and remaining Windows validation

`make test-hvf` includes 1241 portable model checks, native HVF tests, CLI
validation, and ACPI/device-tree checks. `make smoke-arm-hyperv` boots an
original EFI probe with the interface off and on. It executes both HVC ABIs,
reads/writes registers, uses actual GPA buffers, rejects an unknown call and
powers off through PSCI. Guest results and counters agree in both modes.
This synthetic probe does not establish Windows consumption.

`--hyperv-stats FILE` writes JSON after initialization, on SIGUSR1 and at
normal exit/reset. An unwritable destination at startup rejects the launch.
Later write failures emit a warning and leave the guest running; subsequent
captures retry. Shutdown still saves the variable store if diagnostics fail.
`FILE.bootN.json` preserves each boot before recreation.
Counters distinguish discovery, both hypercall ABIs, disabled attempts, call
codes, register reads/writes and failures. Tables are bounded and omissions
are counted. Disabled mode records attempts without exposing the interface.
Guest ID/counters establish actual calls, not performance benefit.

No Windows use or performance improvement is implied by those synthetic tests.

## Windows observation, 2026-10-25

Independent APFS clones of the same stopped local system disk and variable
store were used for on/off runs with 8 GiB RAM and one VP. The original system
overlay was not attached to either experiment. The off run reached the Windows
lock screen; `build/windows11-hyperv/off/hyperv.json` records zero discovery or
Hyper-V calls.

The first basic run recorded 45 legacy HVC #1 calls, including 41 Gets and
three accepted guest-identity Sets. Windows read features, recommendations,
hardware features and isolation configuration. Its guest ID was
`0x0001040a000065f4`: Microsoft NT, version 10, kernel build 26100. The installed
edition's branding/build remains 26300; these are different observations.
No SMCCC call or UID query was observed: discovery was through the FADT.

That initial run did not boot successfully. An unsupported extended-capability
query (0x8001) was tolerated, but GetVpRegisters for 0x90015 failed. Repeated
live captures showed the same PC and unchanged disk/network counters.
Inspection of the original kernel with its matching Microsoft public symbols
identified that PC as the terminal loop in HalReturnToFirmware.

The kernel's HvlpInitializePartitionInfoPage first reads 0x90015, allocates a
page and writes its GPA with enable bit 0. Observed consumers read its initial
32-bit pending-flush flag before deciding whether another hypercall is needed.
This inspection motivated the real guest-RAM registration above, rather than
returning a fabricated address. The model tests and EFI probe verify page
registration, clearing, readback and rejected updates. A fresh Windows clone
with page registration accepted the GPA `0x4000d001`, then successfully read
VP index. It reached the next missing register, TLBI control (0x90016), and
halted again. The control's identifier is independently confirmed in Microsoft's
mu_msvm guest ABI header. Kernel inspection also shows mandatory registration
of a separate synthetic exception vector (0x40400).

The per-VP model now retains and validates both registrations. Its native tests
cover isolation between VPs, reserved/alignment errors, early vector access and
reset state. The EFI probe executes architectural TLBI after opting in and
verifies that synthetic-vector registration leaves VBAR_EL1 unchanged. A fresh
Windows clone with these boot registers reached its lock screen, captured in
`build/windows11-hyperv/basic-vp/console.png`. The counters show accepted page,
TLBI and synthetic-vector registrations and successful VP-index queries.

That working boot also exposed unsuccessful interface-version reads, now
implemented above, and frequent SyncContext (0x19) calls. SyncContext and the
extended capability query (0x8001) remain unsupported and return errors.
Windows tolerates these rejections and boots, but this is an experimental
subset: successful core use must not be reported as full Hyper-V support or
a performance improvement.

Use `python3 tools/verify_windows_hyperv.py REPORT --baseline OFF_REPORT` to
audit core register consumption. It requires Windows NT kernel identity,
successful register operations, enabled page/per-VP state, complete consistent
counters and an inactive baseline. It lists unsupported calls even when core
use passes. It explicitly does not verify GUI boot or performance; inspect
the live framebuffer separately. Fixtures reject synthetic-probe identity,
missing/failed operations, bad state, counter mismatches and an active baseline.

The final restart reached the Windows lock screen with 8 GiB RAM and Hyper-V
basic enabled. The frozen report is
`build/windows11-hyperv/basic-final/core-evidence.json`, SHA256
`89386140a0fca9dec80f340917d1c48a4d6ac45ea67bed825dcb37405c52f1c6`;
the off baseline hash is
`cbfeb8ff3fc0a67ef0f97d3d15afe4af5680a768bb835c57faaff7f64f0e9fbb`.
The audit returns `verifiedCoreUsage: true`. It records 1028 Gets and six Sets,
all successful, including four successful `Hv#1` interface reads. The separate
live framebuffer capture `basic-final/console.png` was visually inspected and
shows the lock screen. Its hash and exact active launch paths are retained in
`basic-final/launch.json`.

There were also 48066 rejected SyncContext calls and one rejected extended
capability query. These remain visible in the passing *core-use* report; the
verifier deliberately does not claim complete Hyper-V compatibility or a
measured speedup. Synthetic ESR is implemented but was not queried by Windows
in this observation. SMCCC transport/UID discovery passed the EFI probe, while
Windows used FADT discovery and legacy HVC #1 only.

The running guest uses the private local SSD overlay
`media/windows11-arm64/hyperv-basic-vp.pcd` and variables under
`build/windows11-hyperv/basic-final`. The original system overlay and variables
are preserved. Superseded experiment overlays were removed after their guests
stopped; their diagnostic reports remain. The default Hyper-V mode remains off.

## Primary references

- [TLFS discovery](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/feature-discovery)
- [Hypercall conventions](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface)
- [GetVpRegisters](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallgetvpregisters)
- [SetVpRegisters](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallsetvpregisters)
- [Register identifiers](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name)
- [Privilege mask](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_partition_privilege_mask)
- [Microsoft OpenVMM ARM64 ABI](https://github.com/microsoft/openvmm/blob/main/vm/hv1/hv1_hypercall/src/aarch64.rs)
- [Microsoft OpenVMM parameter parsing](https://github.com/microsoft/openvmm/blob/main/vm/hv1/hv1_hypercall/src/support.rs)
- [Microsoft mu_msvm ARM64 guest register identifiers](https://github.com/microsoft/mu_msvm/blob/main/MsvmPkg/Include/Hv/HvGuestHypercall.h)

## SyncContext investigation, 2026-10-25

At the start of this investigation SyncContext was unsupported. Microsoft's [OpenVMM WHP definitions](https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/whp/src/abi.rs)
identify SyncContext/Ex as a separate ARM64 facility. The WHP synthetic-feature
bit (39) is not a TLFS partition privilege and must not be copied into this
implementation's privilege mask.

Inspection of the unmodified Windows kernel with its matching public PDB
narrows the observed operation down to instruction-context synchronization:

- `KeSweepIcacheCompleteBatch` calls `HvlSyncContext` after cache maintenance
  and DSB/ISB. Its alternative path dispatches work through the kernel's IPI
  machinery. This supports a cross-processor instruction-synchronization
  interpretation; it is not an idle/yield operation.
- `HvlSyncContext(NULL)` constructs fast hypercall 0x10019 with first input 1
  and second input 0. A non-null argument is converted into a VP affinity mask.
- The helper used by `HvlpFastSyncContextEx` and `HvlpSlowSyncContextEx` is
  `HvlpAffinityToHvProcessorSet`. It serializes a processor set, not an ARM
  register context. The slow path falls back to 0x19 if that set cannot fit.

These are observations of this kernel, not a complete specification of flags,
completion guarantees or all supported input layouts. Merely stopping host
vCPU threads or issuing a host memory fence does not establish the required
instruction synchronization on every target guest VP. [Arm's Issue L.a known issues](https://documentation-service.arm.com/static/67ab274f6dbc975ccea916d7)
also clarify that HVC entry is not unconditionally an instruction-fetch barrier:
with FEAT_ExS, the relevant EIS/EOS controls matter. Treating every HVC exit as
a sufficient synchronization operation would therefore be unjustified.
Before accepting this call, the backend needs a justified guest execution synchronization mechanism
and a test that replaces executable guest instructions, synchronizes the target
VPs and checks that they execute the new code, including a multi-VP case.

Research artifacts are ignored under `build/hyperv-research/kernel/`.
The source kernel came from image 1 of the existing installation WinPE WIM;
its matching Microsoft symbol identifier is
`ntkrnlmp.pdb/7A9F162A39E89C67CD02CF2126D51A651`.
Relevant kernel RVAs are `KeSweepIcacheCompleteBatch` 0x259b30,
`HvlSyncContext` 0x454178, `HvlpAffinityToHvProcessorSet` 0x23c810,
`HvlpFastSyncContextEx` 0x626078 and `HvlpSlowSyncContextEx` 0x626100.
These addresses are version-specific and are not runtime dependencies.


## Partition reference counter

`HvRegisterTimeRefCount` (0x90004) now provides partition uptime in 100 ns units,
with the AccessPartitionReferenceCounter privilege (bit 1) advertised. It is
available before GuestOsId, shared across VPs and read-only; writes fail with
invalid-parameter status. The clock starts at partition creation and uses
`steady_clock`, so host wall-clock corrections cannot move it. Equal-resolution
samples advance one tick to preserve strictly increasing reads across VPs.
A new VM starts a fresh epoch. WFI does not suspend the clock; explicit VM
suspend/save/restore is not implemented by this runner.

Deterministic model tests cover exact one-second scaling, VP sharing, both fast
ABIs, equal/backward source samples, early access, rejected writes and reset.
The EFI probe reads in both ABIs around a 10 ms EFI Stall and checks the elapsed
100 ns count. The last returned counter is reported as `referenceTime100ns`;
register operation counters establish whether Windows actually reads it.
This does not provide a reference TSC page or synthetic timer interrupts.

Contract sources: Microsoft's [timer specification](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers),
[register identifiers](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name)
and [ARM64 early-register access rules](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallgetvpregisters).

Windows validation with the counter advertised reached the lock screen on an
independent local APFS disk clone, using 8 GiB and one VP. The snapshot under
`build/windows11-hyperv/reference-clock/core-evidence.json` passes the core
usage audit and records no reads of 0x90004 (`referenceCounterObserved: false`).
Disk and network activity continued. This validates compatibility during this
boot, not Windows use of the new clock or a performance gain. Evidence SHA-256:
`4a8dc5d18ce22e675fa5e5aa400684c5d42889e345e5f7d5496bdcff6c0c5db0`.


## Crash enlightenment

Crash parameter registers 0x210..0x214 are independent per VP and available
before GuestOsId. Register 0x215 reads the supported CrashNotify/CrashMessage
capabilities. A notification copies the five parameters and, when requested,
up to 4096 bytes from guest RAM. Subsequent guest writes cannot change the
captured event. The platform immediately emits a `Hyper-V guest crash:` JSON
record to stderr; the statistics retain a bounded last-event snapshot and a
total notification count. Multiple notifications in one repeated register call
are coalesced into that last snapshot. Notification does not stop the guest or
prevent its own crash-dump procedure.

Messages are hex encoded, preserving arbitrary bytes without terminal control
sequences. Invalid sizes, inaccessible ranges, reserved control bits and nonzero
upper register halves fail before publishing an event. PreOSId and NoCrashDump
metadata are retained in the reported control word. A zero control write is a
no-op; a message request without CrashNotify is invalid. Capability reads always
return the supported actions, not the most recent command.

Feature discovery uses ARM64 bit 72 of 0x200, following the
[ARM64 discovery table](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/feature-discovery).
Microsoft's [partition page](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/partition-properties)
instead says bit 105, contradicting that table. In the original Windows kernel,
HviGetHypervisorFeatures (RVA 0x5cad20) copies the register result unchanged;
the initialization routine (RVA 0x4cef18) checks bit 8 of its upper 64-bit half
before enabling the crash path. HvlLogGuestCrashInformation (RVA 0x4c9070)
then accesses 0x210..0x215. This confirms the bit-72 interpretation for the
validated Windows kernel. The register IDs also match Microsoft's mu_msvm
HvGuestHypercall.h. Control fields follow the
[crash control definition](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_crash_ctl_reg_contents).

The native EFI probe advertises/discovers the capability, submits parameters
and a RAM message, mutates that RAM afterwards and still reaches its success
marker and PSCI shutdown. The host log and persisted JSON must contain the
original message. This proves the notification path without deliberately
crashing the installed Windows guest.

## Remaining implementation and verification scope

The objective remains all relevant extensions for this ARM64 Windows platform.
The following is a work inventory, not a completion claim:

| Facility | Current evidence / remaining work |
| --- | --- |
| Discovery, identity and VP registers | Native EFI and actual Windows register use verified. |
| Partition reference counter | Rate and ABI tested; no Windows reads in the observed boot. |
| Crash enlightenment | Model and native notification/logging tested; actual Windows crash delivery remains unverified. |
| SyncContext / SyncContextEx | Fixed/Ex, fast/memory inputs implemented with real per-VP barriers. Windows fixed-path and native Ex tests pass; Windows Ex use remains unobserved. |
| Extended-capability query 0x8001 | Native memory/fast ABI and two successful Windows queries verified. |
| Remote TLB flushes, processor masks and synthetic IPIs | Fixed/Ex address-space flushes, ARM drain and IPIs are native-tested. Windows fixed IPI use verified; Windows TLB/Ex use and ARM list applicability still need verification. |
| VP assist and reference-time page | Shared-memory ABI, ordering and guest consumption pending. |
| SynIC and synthetic timers | Event/message controller, four timers per VP and native PPI delivery tested, still unadvertised; connections, complete ARM semantics and Windows consumption pending. ARM Generic Timer already runs natively. |
| VMBus / integration services | Relevance and Windows driver consumption still need evaluation against the VirtIO platform; not implemented. |
| Nested virtualization / isolation | Host/backend capability and applicability not established; not advertised. |

The Windows loopback/DHCP proof, on/off baseline and GUI boot evidence do not
prove any of the unfinished services above. Every enabled feature needs its
own side-effect tests and guest consumption evidence; performance claims need
an actual baseline comparison.


## Extended capability discovery

HvExtCallQueryCapabilities (0x8001) is supported and advertised with the
EnableExtendedHypercalls privilege (bit 52). It takes no input and returns an
8-byte capability bitmap. The bitmap is currently zero: none of the services
0x8002..0x8006 is provided. Unknown extended services continue to fail.

Both memory and fast transports use the existing ARM64 conventions. In memory
transport only eight output bytes are written, with the input pointer ignored.
In fast transport the result is X2/X3 for SMCCC and X15/X16 for legacy calls;
the unused upper half of the output pair is zeroed. Reserved control bits,
rep counts/start indices, invalid VP indices and invalid output addresses fail
without publishing output. Guest identity is required.

Each hypercall counter now includes its failure count. The Windows auditor
reports `extendedCapabilitiesObserved` only for a valid Windows core trace
with at least one query and zero query failures. Old snapshots without those
per-call counters cannot establish successful query consumption.

Sources: Microsoft's [query contract](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvextcallquerycapabilities)
and the OpenVMM `HvExtQueryCapabilities` SimpleHypercall<(), u64> definition.
The native probe validates actual GPA output, preserved adjacent bytes and
fast legacy output. Model tests additionally cover both transports in both
ABIs, unused invalid input pointers, malformed controls and failed identity.

The separate Windows run with crash enlightenment enabled reached the lock
screen and passes the core usage auditor. No crash notification was recorded,
as expected for a normal boot. This proves compatibility, not an actual Windows
bugcheck notification. Its frozen report is
`build/windows11-hyperv/crash-enlightenment/core-evidence.json` (SHA-256
`e7c20148a37002639814a86f31356877676c6fadd8054b69260a4cbeb5082f0e`).

Windows with extended queries enabled reached the lock screen on the local
8 GiB, one-VP clone. The frozen report under
`build/windows11-hyperv/extended-query/core-evidence.json` records two 0x8001
queries with zero failures; `extendedCapabilitiesObserved` is true. Core usage
also passes, with continued disk/network activity. Report SHA-256:
`7051fc988ecd62f6b9c61f2fb9afd87204e560c9ea326e8b43aade6349714598`.


## Fixed fast SyncContext

The observed Windows fast 0x19 ABI is implemented: two input words contain
flags and a VP mask. Flag 1 selects all configured VPs and requires a zero mask;
flag 0 selects the supplied mask. Guest identity is required. Reserved flags,
unknown VPs, variable headers, rep fields and non-fast layouts are rejected.
Both SMCCC and legacy transports preserve other argument registers. A missing
backend returns invalid-code; a backend reporting incomplete work returns
operation-failed (0x71). Exceptions from the native backend stop the runner
rather than acknowledging a barrier that did not complete.

This fixed layout is reconstructed from the original Windows HvlSyncContext
routine described above; it is not a claim to implement the undocumented
SyncContextEx or slow-call layouts. The follow-up implementation below adds the Ex and memory layouts.

The HVF backend maps a private read/execute-only stub below RAM, at GPA
0x3fff0000. For each selected online VP it temporarily selects physical EL1h
execution, masks interrupts, debugger activity and virtual-timer exits, and
runs `DSB SY; ISB; HVC #0xfffe` on that VP's own HVF owner thread. Completion
requires both the sentinel HVC and the exact expected PC. It then restores
PC, PSTATE, SCTLR, VBAR, ELR, SPSR, ESR, FAR, MDSCR and the timer mask. The stub
uses no GPRs or guest stack. Its private vectors catch unexpected EL1 exceptions
without entering the guest's normal handler. All other VPs remain stopped.
The existing watchdog bounds each attempt to 250 ms; errors restore the saved
context and do not increment completion counters. Offline/never-started VPs
have no old instruction stream and are skipped.

The backend counter `instructionContextBarriers` records completed execution
per VP. The model records successful requests/last target mask separately.
`contextSyncObserved` requires successful Windows calls, matching request
counts and actual native barrier counts; a model counter alone is insufficient.

Native tests execute changed code on two VPs, verify target masks and preserved
GPR/PC/system context, inject both a breakpoint and an infinite loop into the
stub, and verify rollback/recovery on both owner threads. Another test covers
an online VP in WFI. The EFI probe executes a self-modified RAM function with
MMU enabled, synchronizes through the real 0x19 HVC and verifies the new result,
SCTLR and VBAR before continuing to crash-report and PSCI tests. These tests
establish the backend and guest ABI; Windows consumption still needs a fresh
run with this implementation.

Architecture rationale: Arm's [cross-thread self-modifying-code explanation](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/caches-self-modifying-code-working-with-threads)
requires an instruction synchronization event on executing cores. The explicit
ISB path avoids depending on undocumented HVF exception-entry EIS/EOS choices
or mistaking a host memory fence for a guest instruction barrier.


## SyncContextEx and memory transport

0x1a accepts a fixed 24-byte input (zero flags and HV_VP_SET's format and
ValidBanksMask), followed by variable bank-content words. The control's
10-bit variable-header length must equal popcount(ValidBanksMask). Format 1
selects all configured VPs with no bank array. Format 0 supports sparse banks;
zero-valued banks are legal even above this platform's VP range, while nonzero
bits selecting nonexistent VPs fail. Unknown formats/flags fail before invoking
the backend. The standard 128-byte fast-input limit and 4 KiB GPA input-page
bounds apply. Both ARM64 ABIs support memory and fast input. Fixed 0x19 now
also supports its 16-byte memory input, with the same flags/mask semantics.

These layouts match the original Windows HvlpFastSyncContextEx and
HvlpSlowSyncContextEx routines (RVAs 0x626078/0x626100), which serialize a VP set
with HvlpAffinityToHvProcessorSet and put only bank-content length in the
variable-header field. The [HV_VP_SET specification](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_vp_set)
and [hypercall header/transport rules](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface)
define the common encoding. The ExProcessorMasks recommendation remains off
until its other dependent TLB/IPI interfaces are ready; implementing this call
alone does not justify recommending all Ex interfaces.

Model tests cover all four ABI/transport combinations, all/empty/sparse sets,
64 banks with out-of-range empty entries, malformed lengths and reserved bits,
invalid VP bits and GPA boundaries above 4 GiB. The native EFI probe executes
fixed and Ex calls around its self-modified function with MMU enabled; all four
requests must correspond to four completed native barriers. The Windows audit
now includes both call codes in its barrier/request comparison.

The separate fixed-SyncContext Windows run reached the lock screen, with disk
and network traffic continuing. Its frozen snapshot records **50733 successful
SyncContext calls and 50733 actual VP0 barriers**, 1038 register calls and two
successful extended-capability queries. All 51773 hypercalls have zero failures.
`contextSyncObserved` and core usage both pass. This establishes actual Windows
consumption of the fixed path; no Ex call or performance improvement is implied.
Evidence: `build/windows11-hyperv/sync-context/core-evidence.json`, SHA-256
`36da58a21e54b2e2644f6e5dca4bb01f7f9427fe93f69fe67ece76f1c7e9059b`.


## Two-VP Windows and translation-maintenance backend

Windows with two VPs and 8 GiB reached the lock screen and passed the core and
SyncContext usage audit. The frozen snapshot records 53004 successful fixed
SyncContext calls, with **50897 VP0 and 36109 VP1** completed barriers, 1048
register calls and two successful extended queries. All 54054 calls have zero
failures. Both VPs opted into TLBI control and registered their synthetic vector.
The Ex path remains unobserved in Windows. This is compatibility and actual
multi-VP use evidence, not a performance comparison against the one-VP off run.
Evidence: `build/windows11-hyperv/sync-ex-smp/core-evidence.json`, SHA-256
`77666f81f8b86eb39530d317c6ac72174a25144df6bec711f986dc71353dc5a5`.

The HVF backend also provides `flushTranslationContext(mask)`. It uses the
same saved-context/owner-thread/watchdog mechanism as SyncContext, but executes
`DSB SY; TLBI VMALLE1IS; DSB SY; ISB` before its separate completion sentinel.
This conservatively invalidates all stage-1 EL0/EL1 translations, including
all ASIDs/global entries; the shareable TLBI can evict translations beyond
the requested VP mask. Each selected online VP completes maintenance before
return. `translationContextFlushes` counts completed executions separately
from instruction-only barriers. No stage-2 flush is implied.

A real two-VP test installs 4 KiB translation tables, enables the MMU and
primes a globally mapped virtual alias. The host changes the alias PTE to a
different physical page without any guest TLBI. After backend maintenance,
both VPs must read the new physical-page value. Tests also check preserved
TTBR/TCR state, target masks, distinct counters, an invalid mask and a corrupted
completion sentinel with recovery. The native self-test now has 193 checks.

This backend primitive is groundwork for the Hyper-V TLB hypercalls. Their input parsing and guest consumption were pending at this checkpoint;
the following section adds the address-space calls. List/repetition variants
and guest advertisement/consumption remain pending. The current Windows binary was launched before this backend
change and does not claim Hyper-V TLB-flush use.

Architecture reference: Arm's [TLB maintenance description](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/LearnTheArchitecture-MemoryManagement-101811_0100_00_en.pdf)
and Microsoft's [address-space flush contract](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallflushvirtualaddressspace).
The latter explicitly allows global entries to be invalidated even when only
non-global mappings are requested.


## Hyper-V address-space flush hypercalls

HvCallFlushVirtualAddressSpace (0x2) and HvCallFlushVirtualAddressSpaceEx
(0x13) now invoke the verified native translation-maintenance backend. The
fixed input has AddressSpace, Flags and ProcessorMask. Ex has AddressSpace,
Flags and HV_VP_SET, with only bank contents counted in the variable header.
Both ARM64 ABIs support register and memory input. The existing VP-set parser
is shared with SyncContextEx; its transport/page bounds and input validation
also apply here.

Flags 0..7 implement ALL_PROCESSORS, ALL_VIRTUAL_ADDRESS_SPACES and
NON_GLOBAL_MAPPINGS_ONLY. ALL_PROCESSORS ignores processor bits, including
bits selecting nonexistent VPs, while malformed VP-set syntax still fails.
Without that flag, nonexistent VP selections fail. Unknown flag/control bits
fail before dispatch. AddressSpace is recorded for diagnostics, but the native
backend conservatively invalidates all address spaces and global mappings;
this covers every requested subset and does not claim selective-flush speed.
A missing backend or incomplete backend operation cannot return success.

The model reports successful `tlbFlushRequests`, last target mask, requested
address space and flags. Native `translationContextFlushes` count completed
maintenance independently. The Windows auditor's `tlbFlushObserved` requires
successful 0x2/0x13 calls, matching request counts and real native completions.
No Windows use has yet been observed; the running two-VP Windows instance
predates this hypercall implementation.

Model tests cover every flag combination in both transports/ABIs, ignored
invalid masks for ALL_PROCESSORS, sparse/all processor sets, reserved fields,
input bounds, missing/failed backend and preserved argument registers. The EFI
probe submits a fixed fast SMCCC call and an Ex memory legacy call with the MMU
enabled. Both must execute real TLBI and preserve SCTLR, VBAR and the function's
result; host counters must report two flush requests and two completions.
The separate two-VP page-remapping test verifies the actual translation effect.

Sources: Microsoft's [fixed address-space contract](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallflushvirtualaddressspace),
[Ex contract](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallflushvirtualaddressspaceex)
and [processor-set encoding](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_vp_set).
ExProcessorMasks remains unadvertised until the other dependent interfaces
are implemented and verified. List flushes/repetition semantics and synthetic
IPIs are still outstanding.


## ARM64 list-flush and IPI investigation

Microsoft explicitly labels [FlushVirtualAddressList](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallflushvirtualaddresslist)
and [FlushVirtualAddressListEx](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallflushvirtualaddresslistex)
as x64-only. The original ARM64 kernel's HvlFlushRangeListTb (RVA 0x453fc8)
uses VslpEnterIumSecureMode and VslSlowFlushSecureRangeList (RVAs
0x23d668/0x23d590), not a verified 0x3/0x14 call sequence. Therefore those list
contracts must not be copied into ARM64 merely to fill the x64 feature list.
ARM64 list applicability remains an open investigation; no support is claimed.

HvlSendSyntheticClusterIpi (RVA 0x4d1100) does issue fast 0xb, and its Ex helper
exists at RVA 0x4d1888. The processor-set transport can reuse the existing
parser, but the actual ARM64 vector/delivery contract still needs confirmation.
In particular, the generic IPI page describes vectors 0x10..0xff, whereas GIC
SGIs have IDs 0..15; inventing an offset between them would be unjustified.
Microsoft's [ARM64 interrupt controller description](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/virtual-interrupts)
specifies a virtual GIC. The registered synthetic exception vector is not a
substitute IRQ injection path: KiSyntheticException (RVA 0x2b6e40) dispatches
synthetic syndrome classes to VTL/secure/virtualization-fault handlers.

The native backend now provides `sendSoftwareInterrupt(mask, sgi)`, which pends
GIC SGI IDs 0..15 on the selected configured VPs. It validates the mask and SGI
before any write, requires GICv3, runs each redistributor API on its vCPU owner
thread and preserves other pending bits. It does not rewrite the guest's
interrupt enables, groups or priority controls. `softwareInterruptRequests`
counts requests, not guest handler invocations: repeated requests can coalesce.
At that checkpoint this was delivery groundwork; the functional IPI path below now advertises support.

The real two-VP test configures the GIC in guest code and reads ICC_IAR1_EL1,
then performs EOI. A targeted SGI 5 must appear only on VP1; SGI 7 must appear
on both VPs; separately queued SGI 9 must survive until the preceding SGI has
been acknowledged. Repeated pending SGI 5 coalesces and no extra interrupt may
remain after EOI. Empty/invalid requests cannot change counters, and a VM
without GIC must reject the API. The native self-test now has 224 checks.
The test explicitly acknowledges through IAR with DAIF masked; it does not yet
prove a guest IRQ vector handler or the Hyper-V-to-GIC vector translation.

API authority: Apple's bundled Hypervisor.framework `hv_gic.h` specifies that
redistributor accesses run on the owning thread and use GICv3 register offsets.
The implementation accesses GICR_ISPENDR0 through those APIs on both primary
and secondary threads. Future IPI/SynIC enablement still requires the correct
guest ABI, interrupt-handler tests and Windows consumption evidence.

A later checkpoint of the existing two-VP Windows run still has zero hypercall
failures: 154226 successful SyncContext calls, with 111149 VP0 and 108532 VP1
completed barriers. The core/SyncContext auditor passes. No reference-counter
reads, IPI hypercalls or TLB hypercalls were observed in that binary.
Frozen evidence: `build/windows11-hyperv/sync-ex-smp/soak-evidence.json`, SHA-256
`5628a10a1fe1eee2694a7bc52b3e1f7be2c8c76f2b7e295db62369b97a58c437`.


## Diagnostic Windows IPI recommendation probe

The historical `--hyperv-probe-ipi` option was an explicit diagnostic experiment, requiring
`--hyperv basic`. It sets only the synthetic-cluster-IPI recommendation in
HvRegisterFeaturesInfo and captures the two fixed fast 0xb input words in
`ipiProbe`. IPI requests still return the normal unsupported-call error;
this option does not claim implemented IPI delivery and may prevent a guest
from booting. Use an independent test disk/variable clone, not the working OS.
Normal basic mode leaves this recommendation off and does not collect inputs.

The purpose is to observe which interrupt number Windows ARM64 supplies,
without guessing an APIC-vector-to-GIC offset from the generic x64-oriented
table. The original HvlSendSyntheticClusterIpi routine supplies a 32-bit first
word and a VP mask to fast 0xb; the vector's meaning is the remaining question.
The bounded report stores only those defined input words, count and enabled
state. This experiment has now been removed from the CLI and model; the implementation
below replaces it. Old probe artifacts remain historical evidence only.


The isolated recommendation probe captured two Windows fast 0xb requests.
The frozen last input is word 0 = 0, word 1 = 2: vector/TargetVtl/reserved fields
are zero and the target is VP1. This directly establishes that vector 0 is a
valid Windows ARM64 request, contradicting an unqualified application of the
x64 0x10 lower bound. It is consistent with GIC SGI 0 and needs no invented
vector offset. Delivery is still deliberately rejected in this probe build;
the captured arguments do not themselves prove delivery. Frozen evidence:
`build/windows11-hyperv/ipi-probe/input-evidence.json`, SHA-256
`7302235a91af16987e78b50152d9afadd51857d0bbfb46b9afa0824f9d6f8778`.

## ARM enlightened TLB drain

HvCallFlushTlb (0xd6) now executes a real translation-context flush for the
calling VP. It has no input/output buffers: both GPA arguments are ignored.
Fast/memory transports are accepted in both HVC ABIs; nested, variable-header,
rep and reserved control fields fail. Identity is required, and a missing or
incomplete native backend cannot produce success. Successful drains share the
TLB request/native-completion accounting used by 0x2 and 0x13.

Microsoft's mu_msvm HvGuestHypercall.h calls 0xd6 the ARM enlightened TLB flush.
In the original Windows kernel, the paths at RVAs 0x29c7b0, 0x4353cc and
0x454148 read the partition-info page's initial word and, if nonzero, invoke
0xd6 with zero input/output GPAs. This backend queues no asynchronous flush
work, so that word remains zero; the new call does not fabricate a pending
queue. Its conservative native TLBI still performs actual invalidation when
explicitly invoked. No Windows use is claimed while the pending word is zero.

Model tests cover both transports/ABIs, caller-VP targeting, ignored invalid
GPAs, reserved controls, invalid VP, identity and backend failure. The EFI probe
executes a legacy memory 0xd6 call alongside fixed/Ex address-space flushes;
three successful requests must equal three completed native flushes. The
Windows usage auditor includes 0xd6 in its native-completion check.


## Fixed and Ex ARM64 IPI delivery

HvCallSendSyntheticClusterIpi (0xb) and its Ex variant (0x15) now deliver real
GIC SGIs to the selected VPs. The first input word holds a 32-bit SGI ID and
HV_INPUT_VTL; current VTL0 and explicit VTL0 are supported. When UseTargetVtl
is clear, the target nibble is ignored, matching Microsoft's OpenVMM decoder.
Reserved bits/flags, explicit nonzero VTLs and SGI IDs above 15 fail before
injection. The fixed VP mask and Ex processor set use the same bounds checks
as the other interfaces; both HVC ABIs and fast/memory transports work.

ARM [GIC interrupt IDs 0..15 are SGIs](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/GICv3_v4_overview.pdf).
The original Windows fast 0xb trace supplies ID 0 directly, with no APIC-vector
offset. This ARM64 implementation deliberately does not apply the generic
IPI page's x64-oriented 0x10..0xff range. Packet definitions and HV_INPUT_VTL
semantics are corroborated by Microsoft's OpenVMM hvdef types.

The IPI recommendation (bit 2 of HvRegisterFeaturesInfo) is set only when a
real software-interrupt backend is installed. The diagnostic probe option
has been retired. ExProcessorMasks remains off while broader applicability
and remaining interfaces are assessed. There is no success stub when an
interrupt backend is missing or reports failure.

`ipiRequests` counts successful calls. `ipiTargetRequests` counts requested
VP deliveries and is compared against native `softwareInterruptRequests` by
the Windows auditor's `ipiObserved` gate. These are request counts, not handler
invocation counts; the GIC may coalesce repeated pending SGIs. The latest
mask/vector are recorded. A successful hypercall means the interrupt was
pended through the real GIC API, not that the target handler has finished.

The native two-VP test now also sends SGI 0 to VP1 with interrupts unmasked.
Only VP1 may enter its architectural IRQ vector; its handler reads IAR1,
performs EOI and emits a unique HVC marker. It must then ERET to the interrupted
WFI loop. The native self-test has 228 checks. The EFI probe submits a fixed
fast IPI and an Ex memory IPI, acknowledges IDs 5/7 through IAR1/EOIR1 and
requires matching model/native request counters. Model tests cover all four
ABI/transport combinations, SGI 0/15, VTL selection, processor sets, malformed
inputs, missing/failed backend and preserved argument registers.

Windows 11 ARM64 has now booted to the visually verified lock screen with
the functional IPI path, two VPs and 8 GiB RAM. The frozen snapshot records
65,552 successful fixed IPI calls with SGI 0. Model target requests and native
GIC requests match exactly: VP0 = 36,346, VP1 = 29,206. There are also 53,388
successful SyncContext calls, 1,034 register calls and two extended queries;
all 119,976 hypercalls have zero failures. The auditor passes core usage,
IPI, SyncContext and extended-query checks with no unsupported calls.

Evidence is `build/windows11-hyperv/ipi-enabled/core-evidence.json`, SHA-256
`24cba8e4f2d11f62e08cb1d77bbd5069f310d8c4bb8bb5139f1f361fdb846160`.
The lock-screen capture and launch provenance are in the same ignored local
directory. The run uses a separate local SSD overlay and no installation ISO.
These counters prove Windows consumption and native pending requests, not an
exact count of handler invocations or a performance benefit. Ex IPI, TLB
hypercalls and reference-counter reads were not observed in this snapshot.

## Private-interrupt backend for further enlightenments

The native backend can now edge-pend any private GIC INTID 0..31 on selected
VPs through GICR_ISPENDR0. The SGI-only IPI API retains its 0..15 bounds and
separate request counters. The broader API is a prerequisite for ARM SynIC
and direct synthetic-timer interrupts; it does not advertise those interfaces
or model a continuously asserted level source. The architectural virtual
timer remains managed by its existing backend.

The two-VP native fixture enables private interrupts, injects PPI 16 into VP1
only and PPI 31 into both VPs, and requires the exact IAR1/EOIR1 acknowledgments.
It checks that PPI requests leave the Hyper-V IPI counters unchanged and that
INTID 32 and nonexistent VP targets fail. Existing pending-SGI preservation,
coalescing and actual IRQ-handler/ERET checks still pass. `make test-hvf`
passes with 242 native checks and 1,241 portable Hyper-V checks. This is native
backend evidence; Windows SynIC/timer consumption remains unverified.

## SynIC event and message controller

An initial VTL0 ARM SynIC controller now backs Get/SetVpRegisters for SINT0..15,
SCONTROL, SVERSION, SIFP, SIPP and EOM. Each VP has independent state and starts
disabled with masked SINTs. Page enables require a mapped, aligned 4 KiB RAM
page; a newly installed mapping is cleared, while rewriting the same mapping
retains pending data. Disabled mappings retain their backing RAM. Reserved
fields, version writes and unsupported SIRBP fail rather than acknowledging
an unused response buffer. Identity is required through the hypercall path.

The host event path sets one of 2,048 flags per SINT and requests a real GIC
interrupt only when the bit changes from zero. Masked sources fail without
changing RAM; polling sources set flags without an interrupt. Failed native
injection rolls back the newly set bit. ARM SINT routing currently supports
PPI INTIDs 16..31, consistent with the Linux kernel's
[ARM VMBus interrupt model](https://kernel.org/doc/html/next/virt/hyperv/vmbus.html).
AutoEOI and proxy modes are rejected: the x64 APIC behavior must not be
silently applied to an ARM GIC.

The host message path writes the 16-byte header and up to 240 payload bytes
into the selected 256-byte guest slot before requesting its interrupt. An
occupied slot retains its contents and receives MessagePending; up to 16
additional messages per SINT are retained in FIFO order. A new post or EOM
attempts delivery after the guest clears MessageType. EOM with an occupied
slot leaves it intact, and a masked source can still receive a message in
RAM. Disabled pages defer queued messages. Failed immediate injection restores
the previous slot and removes the newly posted message so callers can retry.
The layouts and register names follow Microsoft's OpenVMM hvdef types and
[SynIC documentation](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication).

`make test-hvf` passes 1,441 portable Hyper-V checks and 247 native checks.
Tests cover duplicate events, flag 2,047, per-VP routing, polling/masking,
page bounds, full queues, FIFO draining, disabled-page retention and backend
failure rollback. The native two-VP fixture routes an actual event through
PPI 16 and messages through PPI 31, requiring GIC IAR1/EOIR1 acknowledgments.

SynIC remains unadvertised. Root port-management hypercalls and complete ARM response-buffer semantics
still require implementation or applicability verification. Windows SynIC
consumption has not been established by these host-driven native tests.


## Synthetic timer implementation

Four per-VP synthetic timers now back ARM registers 0xb0000..0xb0007.
Configuration/count writes implement one-shot absolute deadlines, periodic
intervals, AutoEnable and zero-count cancellation. Message mode with SINT 0
is disabled as specified. Reserved configuration bits, including the ARM
vector field's upper three bits, fail before state changes. A one-shot clears
Enabled on expiration even when its notification must wait for delivery.
Counts and deadlines use the partition reference counter's 100 ns time base.

Direct mode requests the configured GIC INTID 0..31 through the real private
interrupt backend. Message mode uses the standard 24-byte payload (timer
index, reserved zero, scheduled expiration and actual delivery time). Each
pending expiration stays owned by its timer while the SynIC slot is occupied;
it is not repeatedly appended to the generic message queue. A full slot gets
MessagePending. Freeing the slot permits a later service pass to deliver it.
Successful delivery increments `syntheticTimerDeliveries`; this counts native
pending requests or deposited messages, not guest handler invocations.

The VM loop services timers only with all VPs stopped. A non-lazy deadline
shortens the next run slice, rounded up to milliseconds, with a 1 ms retry
budget for blocked delivery. Lazy timers do not force a shorter slice; they
are serviced when the VM loop next regains control. Signals never precede
scheduled expiration. Missed periodic intervals coalesce into one delivery
and retain the original phase for the next deadline. The scheduler extends
sampled 64-bit reference times with an internal epoch. Periodic deadlines can
cross UINT64_MAX without being shortened, dropped or sorted ahead of earlier
deadlines. Guest-visible expiration/delivery values retain their 64-bit ABI.
Sampling must occur at least once per complete counter cycle; backwards clock
steps are interpreted as wraps under the monotonic clock contract.

`make test-hvf` passes 1,596 portable Hyper-V checks and 259 native checks.
Portable tests cover all eight VP/timer combinations, no-early delivery,
periodic catch-up, lazy scheduling, cancellation, invalid register fields,
backpressure and the exact timer-message payload. Native tests require timer
messages to produce PPI 31 on both VPs and a direct timer to produce PPI 17
only on VP1, acknowledged through IAR1/EOIR1. The EFI probe programs an expired
direct timer through SMCCC SetVpRegisters, acknowledges SGI 9, and reads its
disabled configuration through legacy GetVpRegisters. Its message-timer
checks and complete delivery totals are described below.

The register layout and timing behavior follow Microsoft's
[timer contract](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers)
and OpenVMM's ARM vector and TimerMessagePayload definitions. Microsoft
recommends the architectural Generic Timer on ARM64; that existing timer
continues to run independently. Synthetic timer privileges/direct-mode
features remain unadvertised. Actual Windows timer consumption, full SynIC
integration and applicable reset/suspend behavior still require verification
before this facility is considered complete.


## Timer rollover and SynIC rescanning

The scheduler now uses an internal 128-bit timeline while exposing 64-bit
registers and payloads. Relative wake budgets use the same epoch-aware order;
a wrapped low deadline is not mistaken for an overdue timer. Tests cover a
UINT64_MAX-length period, deadlines on either side of rollover, blocked
pre-wrap delivery completed after wrap, missed-period phase retention, and
ordering across VPs. This proves scheduler arithmetic with injected samples;
it does not claim a real partition has run through an entire counter cycle.

SynIC queues are now rescanned between stopped execution slices as well as on
EOM and posting. The native GIC consumes EOI without a hypervisor trap, so a
nonempty, enabled message queue requests a maximum 1 ms run slice. After the
guest clears MessageType, the next service pass deposits the queued message
and pends its interrupt. This is polling after native EOI, not synchronous EOI
interception or a hard wall-clock latency guarantee. Disabled pages stop
forcing short slices; failed injection preserves the slot and queue for retry.
A failed target does not prevent rescanning other queues.

The extended EFI probe allocates a separate third page for SynIC messages.
Two already-due timers target the same PPI/SINT. It verifies the first message
and MessagePending, acknowledges PPI 16, clears MessageType and spins without
HVC/EOM. The second timer message and a second PPI 16 must arrive through the
normal VM run loop. Together with its direct timer, the probe requires three
successful timer deliveries and five private GIC requests (two IPIs and three
timers). Full SynIC/Windows advertisement remains pending.

Validation: `make test-hvf` passes 1,596 portable Hyper-V checks and 259 native
checks, plus the CLI, platform and Windows-auditor suites. The extended EFI
probe passes in both off/basic modes; artifacts for this run are under
`build/hyperv-research/rollover-rescan-smoke.log` and its reported local
`build/hyperv-smoke-*` directory.

## Guest messages and event connections

HvCallPostMessage (0x5c) and HvCallSignalEvent (0x5d) now dispatch to explicit
host-bound endpoints. Bindings are partition-local, unscoped 24-bit connection
IDs, capped at 64. Duplicate IDs, empty callbacks and invalid flag ranges are
rejected. Removing an endpoint does not revoke its partition privilege; later
calls to that ID fail InvalidConnectionId. A callback may remove its own
binding safely. Message payload spans are borrowed only for the handler call.

PostMessage accepts the 256-byte memory packet in either ARM HVC ABI. It
validates reserved fields, nonzero guest message types with bit 31 clear,
payload length up to 240, GPA alignment and page/RAM bounds. Fast PostMessage
is rejected because the fixed packet exceeds this backend's 128-byte fast
input area. SignalEvent accepts memory and fast input in both ABIs and checks
the relative flag against the endpoint's range. Output GPA/pair padding is
ignored because neither call has an output packet. Rep, variable-header,
nested and reserved controls fail before invoking an endpoint.

The host callback's status is preserved, including backpressure and SynIC
errors. A wrong endpoint type fails InvalidPortId. Missing IDs and unsupported
connection scopes fail InvalidConnectionId. No callback means no fabricated
success. The implementation follows Microsoft's
[PostMessage](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallpostmessage)
and [SignalEvent](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallsignalevent)
packets and the corresponding OpenVMM types.

Binding the first message/event receiver grants and reports PostMessages
(privilege bit 36) / SignalEvents (bit 37), respectively. Calls without that
privilege or guest identity return AccessDenied. Normal VM construction binds
no endpoints, so these privileges remain off. This does not advertise SynIC,
create root-partition ports, install a VMBus receiver or claim integration
services. Root port management, scoped IDs and the VMBus protocol remain
separate pending work.

Successful handler acceptance increments `postedMessages` / `signaledEvents`.
The Windows auditor's `messagePostsObserved` / `eventSignalsObserved` gates
require matching positive, failure-free call and acceptance counts with a
valid Windows trace. These gates prove accepted requests, not guest handler
execution, host service completion or a performance benefit.

The native fixture executes an actual fast SMCCC SignalEvent and memory
legacy PostMessage. Host endpoints route them to a SynIC event bit and message
slot; the guest must acknowledge both PPI 16 notifications through IAR1/EOIR1.
Shared RAM is checked for the relative event flag and exact message bytes.
Portable tests additionally cover all SignalEvent ABI/transports, both memory
PostMessage ABIs, malformed packets, deleted/wrong-type endpoints, coalescing,
backpressure, callback self-removal, binding bounds and privilege reporting.

Fast PostMessage rejection is also consistent with OpenVMM's fixed-size
[hypercall definition](https://github.com/microsoft/openvmm/blob/main/vm/hv1/hv1_hypercall/src/imp.rs)
and [fast-input parser](https://github.com/microsoft/openvmm/blob/main/vm/hv1/hv1_hypercall/src/support.rs):
this is a 256-byte simple input, not a variable-size fast packet. Unaligned
GPAs fail InvalidAlignment; straddling/unmapped inputs fail InvalidHypercallInput.

Validation passes 1,758 portable Hyper-V checks, 271 native checks and 11
Windows-auditor tests through `make test-hvf`. The normal off/basic EFI probe
also passes with no host endpoint bindings. An older native fixture's generic
HVC assertion failed intermittently during development, outside the new
connection fixture. The helper now includes caller line, exit reason, PC and
syndrome for diagnosis. Six repeated native runs and the final complete suite
pass; no cause or fix for that intermittent assertion is claimed. Logs are
retained under `build/hyperv-research/connection-*` and `connections-*`.
