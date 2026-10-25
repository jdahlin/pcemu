// Standalone disposable-VM diagnostic for Windows ARM64 HAL/SynIC prerequisites.
// Does not open guest disks or change the emulator's CPU policy.
#include <cstdio>
#if defined(__APPLE__) && defined(__aarch64__)
#include <Hypervisor/Hypervisor.h>
#include <os/object.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
namespace {
void check(hv_return_t result, const char *operation) {
    if (result != HV_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
struct DisposableVm {
    bool created = false, cpuCreated = false, mapped = false;
    hv_vcpu_t cpu = 0;
    void *ram = MAP_FAILED;
    ~DisposableVm() {
        if (cpuCreated) hv_vcpu_destroy(cpu);
        if (mapped) hv_vm_unmap(0x40000000, 16384);
        if (ram != MAP_FAILED) munmap(ram, 16384);
        if (created) hv_vm_destroy();
    }
};
int probe() {
    if (__builtin_available(macOS 15.0, *)) {
        DisposableVm vm;
        auto config = hv_vm_config_create();
        auto configured = hv_vm_config_set_el2_enabled(config, false);
        auto result = configured == HV_SUCCESS ? hv_vm_create(config) : configured;
        os_release(config);
        check(result, "create EL1-only VM"); vm.created = true;
        auto gic = hv_gic_config_create();
        result = hv_gic_config_set_distributor_base(gic, 0x08000000);
        if (result == HV_SUCCESS) result = hv_gic_config_set_redistributor_base(gic, 0x080a0000);
        if (result == HV_SUCCESS) result = hv_gic_create(gic);
        os_release(gic); check(result, "create native GICv3");
        uint64_t typerBefore = 0, typerAfter = 0;
        check(hv_gic_get_distributor_reg(HV_GIC_DISTRIBUTOR_REG_GICD_TYPER, &typerBefore), "read GICD_TYPER");
        // Record readback even when an API write returns success: this register
        // describes hardware capabilities and may remain architecturally read-only.
        auto securityWrite = hv_gic_set_distributor_reg(HV_GIC_DISTRIBUTOR_REG_GICD_TYPER, typerBefore | 0x400);
        check(hv_gic_get_distributor_reg(HV_GIC_DISTRIBUTOR_REG_GICD_TYPER, &typerAfter), "read back GICD_TYPER");
        hv_vcpu_exit_t *exit = nullptr;
        check(hv_vcpu_create(&vm.cpu, &exit, nullptr), "create vCPU"); vm.cpuCreated = true;
        uint64_t pfrBefore = 0, pfrAfter = 0;
        check(hv_vcpu_get_sys_reg(vm.cpu, HV_SYS_REG_ID_AA64PFR0_EL1, &pfrBefore), "read PFR0");
        auto pfrWrite = hv_vcpu_set_sys_reg(vm.cpu, HV_SYS_REG_ID_AA64PFR0_EL1, (pfrBefore & ~0xf00ull) | 0x100);
        check(hv_vcpu_get_sys_reg(vm.cpu, HV_SYS_REG_ID_AA64PFR0_EL1, &pfrAfter), "read back PFR0");
        vm.ram = mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (vm.ram == MAP_FAILED) throw std::runtime_error("allocate guest page");
        auto code = static_cast<uint32_t *>(vm.ram);
        code[0] = 0xd5380400; // MRS x0, ID_AA64PFR0_EL1
        code[1] = 0xd5384241; // MRS x1, CurrentEL
        code[2] = 0xd4000002; // HVC #0
        check(hv_vm_map(vm.ram, 0x40000000, 16384, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC), "map guest code");
        vm.mapped = true;
        check(hv_vcpu_set_reg(vm.cpu, HV_REG_PC, 0x40000000), "set PC");
        check(hv_vcpu_set_reg(vm.cpu, HV_REG_CPSR, 0x3c5), "start EL1h");
        check(hv_vcpu_run(vm.cpu), "run guest probe");
        uint64_t guestPfr = 0, guestEl = 0;
        check(hv_vcpu_get_reg(vm.cpu, HV_REG_X0, &guestPfr), "read guest PFR0 result");
        check(hv_vcpu_get_reg(vm.cpu, HV_REG_X1, &guestEl), "read guest CurrentEL result");
        if (exit->reason != HV_EXIT_REASON_EXCEPTION || (exit->exception.syndrome >> 26) != 0x16 ||
            guestEl != 4 || guestPfr != pfrAfter)
            throw std::runtime_error("guest did not observe the CPU policy and exit through HVC from EL1");
        std::printf("{\"vmEl2Enabled\":false,\"guestEl\":1,\"pfrBefore\":\"0x%llx\","
                    "\"pfrWriteStatus\":%u,\"pfrAfter\":\"0x%llx\",\"guestPfr\":\"0x%llx\","
                    "\"gicTyperBefore\":\"0x%llx\",\"securityWriteStatus\":%u,"
                    "\"gicTyperAfter\":\"0x%llx\",\"securityExtn\":%s,\"guestHvcVerified\":true}\n",
                    static_cast<unsigned long long>(pfrBefore), unsigned(pfrWrite),
                    static_cast<unsigned long long>(pfrAfter), static_cast<unsigned long long>(guestPfr),
                    static_cast<unsigned long long>(typerBefore), unsigned(securityWrite),
                    static_cast<unsigned long long>(typerAfter), (typerAfter & 0x400) ? "true" : "false");
        return 0;
    }
    std::fprintf(stderr, "This probe requires macOS 15 or later.\n"); return 2;
}
}
int main() {
    try { return probe(); }
    catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
#else
int main() { std::fprintf(stderr, "This probe requires Apple Silicon and Hypervisor.framework.\n"); return 2; }
#endif
