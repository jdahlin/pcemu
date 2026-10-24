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

Feature registers expose guest identity, VP index, register access and fast
output. No timer, SynIC, VMBus, flush or scheduling facilities are advertised.
The version is PCEmu interface 1.0 build 1, not a Microsoft release identity.
Unsupported operations/registers return errors rather than fabricated success.

Fast transport follows Microsoft's OpenVMM 16-byte register-pair layout:
SMCCC output follows input pairs; legacy output uses the final pairs ending
at X16. A single-register Get returns X6/X7 and X15/X16 respectively.

## Evidence and remaining Windows validation

`make test-hvf` includes 101 portable model checks, native HVF tests, CLI
validation, and ACPI/device-tree checks. `make smoke-arm-hyperv` boots an
original EFI probe with the interface off and on. It executes both HVC ABIs,
reads/writes registers, uses actual GPA buffers, rejects an unknown call and
powers off through PSCI. Guest results and counters agree in both modes.
This synthetic probe does not establish Windows consumption.

`--hyperv-stats FILE` writes JSON after initialization, on SIGUSR1 and at
normal exit/reset. `FILE.bootN.json` preserves each boot before recreation.
Counters distinguish discovery, both hypercall ABIs, disabled attempts, call
codes, register reads/writes and failures. Tables are bounded and omissions
are counted. Disabled mode records attempts without exposing the interface.
Guest ID/counters establish actual calls, not performance benefit.

Windows on/off boots and register counters remain to be inspected. No Windows
use or performance improvement is implied by those synthetic tests.

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
is being tested with that implementation; successful on-mode boot is still
unproven at this checkpoint.

## Primary references

- [TLFS discovery](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/feature-discovery)
- [Hypercall conventions](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface)
- [GetVpRegisters](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallgetvpregisters)
- [SetVpRegisters](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/hvcallsetvpregisters)
- [Register identifiers](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name)
- [Privilege mask](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_partition_privilege_mask)
- [Microsoft OpenVMM ARM64 ABI](https://github.com/microsoft/openvmm/blob/main/vm/hv1/hv1_hypercall/src/aarch64.rs)
- [Microsoft OpenVMM parameter parsing](https://github.com/microsoft/openvmm/blob/main/vm/hv1/hv1_hypercall/src/support.rs)
