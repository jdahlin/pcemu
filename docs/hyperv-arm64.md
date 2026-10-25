# Experimental ARM64 Hyper-V interface

`--hyperv basic` enables the initial ARM64 discovery/register interface.
The default is `off`. This independent TLFS subset does not provide VMBus,
SynIC, synthetic timers, TLB-flush hypercalls or nested virtualization.

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

`make test-hvf` includes 446 portable model checks, native HVF tests, CLI
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
| Remote TLB flushes, processor masks and synthetic IPIs | Multi-VP implementation and guest validation pending. |
| VP assist and reference-time page | Shared-memory ABI, ordering and guest consumption pending. |
| SynIC and synthetic timers | Interrupt delivery and timer expiration implementation/validation pending; ARM Generic Timer already runs natively. |
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

This backend primitive is groundwork for the Hyper-V TLB hypercalls. Their
input parsing, flags, repetition semantics and guest advertisement/consumption
are still pending. The current Windows binary was launched before this backend
change and does not claim Hyper-V TLB-flush use.

Architecture reference: Arm's [TLB maintenance description](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/LearnTheArchitecture-MemoryManagement-101811_0100_00_en.pdf)
and Microsoft's [address-space flush contract](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallflushvirtualaddressspace).
The latter explicitly allows global entries to be invalidated even when only
non-global mappings are requested.
