# Machine profiles and execution backends

Use `build/pcemu --list-machines` to see available and planned profiles.
The machine profile selects a guest platform and defaults; `--backend` selects
execution independently. No new disks or installed guests are created by choosing
an ID. Installations and checkpoints remain user-owned VM instances.

| Profile | Platform | Default RAM | Status |
|---|---|---:|---|
| pc-legacy | Current 32-bit x86 PC | 16 MiB | Available; existing default |
| win311 | Same PC, Windows 3.11 preset | 16 MiB | Available |
| win95 | Same PC, Windows 95/Office 97 preset | 32 MiB | Available |
| win11-arm64 | ARM64 UEFI virtual machine | 4096 MiB | Planned; launch rejected |
| win11-x64 | x86-64 UEFI PC | 4096 MiB | Planned; launch rejected |

The available presets share hardware and CPU semantics. They are not 386/486/
Pentium timing models, do not change CPUID, and do not hide devices. BIOS services,
IDE, PIC/PIT, VGA/DISPI, PS/2, PCI and NE2000 remain available. Add a distinct
versioned platform when guest-visible hardware changes. Historical years alone
are insufficient to identify a compatible machine.

```
build/pcemu --machine win311 --backend interpreter --disk media/win311.raw
build/pcemu --machine win95 --backend jit --load-snapshot build/snapshots/office97-networked
```

`auto` currently selects the existing JIT path (including its interpreter
fallback). `interpreter` disables JIT; `jit` selects that path explicitly.
`--no-jit` is a compatibility alias and conflicts with an explicit other backend.
`hvf` and `kvm` are reserved names and fail before a machine or disk is opened;
no hardware backend is implemented. `--ram-mb` overrides preset RAM, within the
current 2–64 MiB limit. Restoring a checkpoint supplies its saved RAM and device
state, overriding preset and explicit RAM. All available presets use the same
`pc-legacy-v1` platform and can restore existing checkpoints. State JSON records
requested preset, actual backend, platform and actual RAM. Existing checkpoint
formats are unchanged and do not yet encode these profile IDs.

## Implementation path

1. Keep Windows 3.11/95 correctness and startup benchmarks on the current legacy
   platform. Compare interpreter/JIT using the same checkpoint and workload.
2. Add a second concrete session when implementing another platform. Share
   lifecycle, normalized input/frame delivery and disk storage with SDL; keep
   backend selection outside instruction and memory-access loops. Do not force
   hypervisor vCPUs into the existing concrete x86 CPU class.
3. For efficient Linux on ARM hosts, implement an ARM64 virtual platform/backend
   as a separate target. A macOS Virtualization.framework adapter is an option
   for Linux. Its device/firmware contract must not be assumed compatible with
   Windows. Linux KVM is a separate host backend.
4. For Windows 11 on Apple Silicon, prioritize ARM64 and Hypervisor.framework.
   This requires ARM firmware, memory map, interrupt/timer model, ACPI tables,
   SMP/vCPU lifecycle, UEFI boot and variables, TPM 2.0 and Secure Boot support.
   Validate the selected firmware and each virtual device with Windows ARM64.
   Storage/network/input/display need supported ARM64 guest drivers; high graphics
   performance additionally needs a Windows graphics driver and host GPU path.
   HVF exposes CPU virtualization, not this complete Windows machine.
5. Windows 11 x64 on ARM requires a new x86-64 emulation implementation: long
   mode, 64-bit paging/registers, required CPU features/instructions, APIC/SMP,
   modern firmware and devices. It cannot use ARM hardware virtualization to
   execute x86 instructions. Keep it a separate platform with its own tests.
6. Before a new platform writes snapshots, version platform, guest architecture,
   device topology, firmware and backend CPU state in checkpoint metadata.
   Validate compatibility before creating private disks; CPU state is not
   automatically portable between emulator and hardware backends. Incremental
   checkpoint ancestry must also match VM/platform identity. Current RAM
   snapshots remain full compressed saves; disk COW already stores deltas.

Windows 11's published baseline includes a compatible 64-bit CPU with at least two
cores, 4 GB RAM, a 64 GB storage device, UEFI/Secure Boot capability, TPM 2.0 and
DirectX 12/WDDM 2.0 graphics. A 64 GB guest disk should be sparse/COW, not a 64 GB
host allocation. Do not allocate that media until a working platform needs it.

References:
- [Microsoft Windows 11 requirements](https://learn.microsoft.com/en-us/windows/whats-new/windows-11-requirements)
- [Apple Hypervisor framework](https://developer.apple.com/documentation/hypervisor)
- [Apple Virtualization framework](https://developer.apple.com/documentation/virtualization)

Validation: `make build/pcemu` then
`python3 -m unittest discover -s tests -p test_machine_profiles.py`.
