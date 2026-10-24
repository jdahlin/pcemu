#pragma once
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pc {
// Guest platform and execution engine are separate choices. These presets do
// not change CPUID or claim cycle-accurate historical CPU/chipset models.
struct MachineProfile {
    std::string_view id, architecture, platform;
    size_t defaultRamMB;
    bool implemented;
    std::string_view description;
    std::string_view runner = "pcemu";
};
inline constexpr std::array machineProfiles{
    MachineProfile{"pc-legacy", "x86-32", "pc-legacy-v1", 16, true,
                   "Current BIOS/IDE/PIC/PIT/VGA/PS2/PCI/NE2000 machine"},
    MachineProfile{"win311", "x86-32", "pc-legacy-v1", 16, true,
                   "Windows 3.11 preset; same legacy hardware, 16 MiB RAM"},
    MachineProfile{"win95", "x86-32", "pc-legacy-v1", 32, true,
                   "Windows 95/Office 97 preset; same legacy hardware, 32 MiB RAM"},
    MachineProfile{"arm64-test", "arm64", "arm-baremetal-v1", 2, true,
                   "Experimental flat EL1 ARM64 test machine; no OS firmware/devices", "pcemu-hvf"},
    MachineProfile{"arm64-uefi", "arm64", "arm-virt-v2", 256, true,
                   "Experimental EDK II UEFI shell, GICv3, PL011, virtio PCI/MMIO and private COW disks", "pcemu-hvf"},
    MachineProfile{"win11-arm64", "arm64", "arm-uefi-v1", 4096, false,
                   "Planned: ARM64 virtualization, UEFI/ACPI/GIC/TPM2 and guest drivers"},
    MachineProfile{"win11-x64", "x86-64", "pc-uefi-v1", 4096, false,
                   "Planned: x86-64 emulation, UEFI/ACPI/APIC/TPM2 and guest drivers"},
};
inline const MachineProfile &machineProfile(std::string_view id) {
    for (const auto &p : machineProfiles)
        if (p.id == id)
            return p;
    throw std::runtime_error("Unknown machine profile: " + std::string(id));
}
enum class ExecutionBackend { Auto, Interpreter, Jit, Hvf, Kvm };
inline ExecutionBackend executionBackend(std::string_view id) {
    if (id == "auto") return ExecutionBackend::Auto;
    if (id == "interpreter") return ExecutionBackend::Interpreter;
    if (id == "jit") return ExecutionBackend::Jit;
    if (id == "hvf") return ExecutionBackend::Hvf;
    if (id == "kvm") return ExecutionBackend::Kvm;
    throw std::runtime_error("Unknown execution backend: " + std::string(id));
}
inline bool useJit(const MachineProfile &profile, ExecutionBackend backend) {
    if (!profile.implemented)
        throw std::runtime_error("Machine profile " + std::string(profile.id) +
                                 " is planned, not implemented: " + std::string(profile.description));
    if (profile.runner != "pcemu")
        throw std::runtime_error("Machine profile " + std::string(profile.id) +
                                 " uses the separate build/" + std::string(profile.runner) + " runner");
    if (backend == ExecutionBackend::Hvf || backend == ExecutionBackend::Kvm)
        throw std::runtime_error("Hardware virtualization is unavailable for this platform; "
                                 "the current x86-32 machine uses interpreter or jit. "
                                 "ARM hosts cannot hardware-virtualize an x86 guest.");
    return backend != ExecutionBackend::Interpreter;
}
} // namespace pc
