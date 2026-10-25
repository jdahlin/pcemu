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

`make test-hvf` includes 156 portable model checks, native HVF tests, CLI
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

SyncContext remains unsupported. Microsoft's [OpenVMM WHP definitions](https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/whp/src/abi.rs)
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
