#include "hvf_machine.hpp"
#include "hvf_secondary.hpp"
#include "arm_gic_ns.hpp"
#if !defined(__APPLE__) || !defined(__aarch64__)
#error "The HVF ARM64 backend requires an Apple Silicon macOS host"
#endif
#include <Hypervisor/Hypervisor.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pc {
namespace {
void check(hv_return_t result, const char *operation) {
    if (result == HV_SUCCESS)
        return;
    std::ostringstream message;
    message << operation << " failed (0x" << std::hex << uint32_t(result)
            << "). Requires Apple Silicon, Hypervisor support and a binary signed with "
               "com.apple.security.hypervisor; use make build/pcemu-hvf.";
    throw std::runtime_error(message.str());
}
}
struct HvfMachine::Impl {
    bool hypervGicEnabled=false;
    std::array<uint32_t,13> gicIdentifiers{};
    std::thread::id owner = std::this_thread::get_id();
    bool gicEnabled = false;
    bool vmCreated = false, mapped = false, vcpuCreated = false;
    void *memory = MAP_FAILED;
    size_t size = 0;
    struct Region { uint64_t address; size_t size; void *memory; };
    std::vector<Region> regions;
    std::vector<std::unique_ptr<HvfSecondary>> secondary;
    std::deque<Exit> pending;
    std::vector<hv_vcpu_t> active;
    unsigned selected = 0;
    bool primaryOnline = true;
    hv_vcpu_t vcpu = 0;
    hv_vcpu_exit_t *exit = nullptr;
    uint64_t exitCount = 0;
    bool contextRomMapped = false;
    std::array<uint64_t, 8> contextBarriers{}, translationFlushes{}, softwareInterrupts{}, privateInterrupts{};
    std::mutex mutex;
    std::condition_variable changed;
    std::thread watchdog;
    bool stopping = false, armed = false;
    uint64_t generation = 0;
    hv_return_t cancelResult = HV_SUCCESS;
    std::chrono::steady_clock::time_point deadline;
    SleepTicks sleepTicks;
    uint64_t sleepBaseline = 0, sleepApplied = 0, nextClockCheck = 0, clockPeriod = 0;
    void cancelLocked() {
        if (active.empty()) return;
        auto result = hv_vcpus_exit(active.data(), uint32_t(active.size()));
        if (cancelResult == HV_SUCCESS) cancelResult = result;
    }
    void cancelAll() { std::lock_guard lock(mutex); cancelLocked(); }
    void leaveCPU(hv_vcpu_t cpu, bool cancelOthers) {
        std::lock_guard lock(mutex);
        std::erase(active, cpu);
        if (cancelOthers) cancelLocked();
    }

    void synchronizeClock() {
        auto absolute = mach_absolute_time();
        if (absolute < nextClockCheck) return;
        nextClockCheck = absolute + clockPeriod;
        const auto total = sleepTicks();
        if (total <= sleepBaseline || total - sleepBaseline <= sleepApplied) return;
        auto advance = total - sleepBaseline - sleepApplied;
        // Paired host reads have sampling jitter. Ignore differences below 10 ms
        // rather than gradually moving the counter while the host is awake.
        if (advance <= clockPeriod / 100) return;
        uint64_t offset = 0;
        check(hv_vcpu_get_vtimer_offset(vcpu, &offset), "get virtual timer offset");
        // CNTVCT = mach_absolute_time - offset. Unsigned subtraction wraps by
        // design, advancing the virtual counter without moving it backwards.
        check(hv_vcpu_set_vtimer_offset(vcpu, offset - advance), "resume virtual clock");
        for (auto &cpu : secondary) cpu->setOffset(offset - advance);
        sleepApplied += advance;
    }

    void requireOwner() const {
        if (std::this_thread::get_id() != owner)
            throw std::runtime_error("HVF vCPU operations must run on their creating thread");
    }
    void initialize(size_t bytes, bool enableGic, unsigned cpuCount, bool hypervGic) {
        if(hypervGic && !enableGic) throw std::runtime_error("Hyper-V GIC view requires native GICv3");
        hypervGicEnabled=hypervGic;
        if (!cpuCount || cpuCount > 8 || (cpuCount > 1 && !enableGic))
            throw std::runtime_error("HVF supports 1..8 CPUs; SMP requires GICv3");
        auto pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0 || bytes < size_t(pageSize) || bytes > size_t(16) * 1024 * 1024 * 1024 ||
            bytes % size_t(pageSize))
            throw std::runtime_error("HVF RAM must be host-page aligned and between one page and 16 GiB");
        if (!HvfMachine::supported())
            throw std::runtime_error("Hypervisor.framework is unavailable (kern.hv_support)");
        hv_return_t created;
        if (__builtin_available(macOS 15.0, *)) {
            auto config = hv_vm_config_create();
            auto configured = hv_vm_config_set_el2_enabled(config, false);
            created = configured == HV_SUCCESS ? hv_vm_create(config) : configured;
            os_release(config);
        } else created = hv_vm_create(nullptr);
        check(created, "hv_vm_create (EL2 disabled)");
        vmCreated = true;
        if (enableGic) {
            if (__builtin_available(macOS 15.0, *)) {
                auto config = hv_gic_config_create();
                auto result = hv_gic_config_set_distributor_base(config, hypervGic ? ArmGicNonsecureView::nativeBase : 0x08000000);
                if (result == HV_SUCCESS)
                    result = hv_gic_config_set_redistributor_base(config, 0x080a0000);
                if (result == HV_SUCCESS)
                    result = hv_gic_create(config);
                os_release(config);
                check(result, "create GICv3");
                gicEnabled = true;
            } else throw std::runtime_error("GICv3 requires macOS 15 or later");
        }
        size = bytes;
        memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (memory == MAP_FAILED)
            throw std::runtime_error("Cannot allocate HVF guest RAM");
        check(hv_vm_map(memory, ramBase, size,
                        HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC), "hv_vm_map");
        mapped = true;
        check(hv_vcpu_create(&vcpu, &exit, nullptr), "hv_vcpu_create");
        vcpuCreated = true;
        mach_timebase_info_data_t timebase{};
        check(mach_timebase_info(&timebase), "get host clock timebase");
        clockPeriod = 1000000000ull * timebase.denom / timebase.numer;
        if (!sleepTicks) sleepTicks = [last = uint64_t(0), sampled = false,
                                      tolerance = clockPeriod / 10000]() mutable -> uint64_t {
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                auto before = mach_absolute_time();
                auto continuous = mach_continuous_time();
                auto after = mach_absolute_time();
                if (after - before > tolerance) continue; // Descheduled during sampling.
                auto absolute = before + (after - before) / 2;
                last = continuous > absolute ? continuous - absolute : 0;
                sampled = true;
                return last;
            }
            if (!sampled) throw std::runtime_error("Cannot sample host sleep clock");
            return last; // Retry next second without disturbing a running guest.
        };
        sleepBaseline = sleepTicks();
        if (enableGic) {
            check(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MPIDR_EL1, 0x80000000), "set MPIDR_EL1");
            if(hypervGic) {
                uint64_t typer=0;
                check(hv_gic_get_distributor_reg(HV_GIC_DISTRIBUTOR_REG_GICD_TYPER,&typer),"Read native GIC interrupt range");
                unsigned count=std::min(1020u,unsigned((typer&31)+1)*32);
                for(unsigned first=32;first<count;first+=32) {
                    unsigned bits=std::min(32u,count-first);
                    auto mask=bits==32 ? UINT32_MAX : (1u<<bits)-1;
                    check(hv_gic_set_distributor_reg(static_cast<hv_gic_distributor_reg_t>(0x80+first/8),mask),"Assign nonsecure SPIs");
                }
                gicIdentifiers=hvfInitializeNonsecureGic(vcpu,exit,0);
            }

            uint64_t features = 0;
            check(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, &features), "get PMU features");
            check(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1,
                                      (features & ~0xf00ull) | 0x100), "enable framework PMUv3");
        }
        uint64_t timerOffset = 0;
        check(hv_vcpu_get_vtimer_offset(vcpu, &timerOffset), "get initial virtual timer offset");
        for (unsigned i = 1; i < cpuCount; ++i)
            secondary.push_back(std::make_unique<HvfSecondary>(i, timerOffset, ramBase + size, hypervGic));
        watchdog = std::thread([this] {
            std::unique_lock lock(mutex);
            while (!stopping) {
                changed.wait(lock, [this] { return stopping || armed; });
                if (stopping)
                    break;
                const auto current = generation;
                const auto target = deadline;
                if (changed.wait_until(lock, target, [this, current] {
                        return stopping || !armed || generation != current;
                    }))
                    continue;
                // Keep vCPU lifetime protected until the cancellation call returns.
                cancelLocked();
                armed = false;
            }
        });
    }
    ~Impl() {
        if (std::this_thread::get_id() != owner)
            std::terminate();
        {
            std::lock_guard lock(mutex);
            stopping = true;
            changed.notify_one();
        }
        if (watchdog.joinable())
            watchdog.join();
        cancelAll();
        for (auto &cpu : secondary) { try { cpu->finish(); } catch (...) {} }
        secondary.clear();
        if (vcpuCreated)
            hv_vcpu_destroy(vcpu);
        for (const auto &region : regions) {
            hv_vm_unmap(region.address, region.size);
            munmap(region.memory, region.size);
        }
        if (mapped)
            hv_vm_unmap(ramBase, size);
        if (memory != MAP_FAILED)
            munmap(memory, size);
        if (vmCreated)
            hv_vm_destroy();
    }
    uint8_t *range(uint64_t address, size_t bytes) const {
        requireOwner();
        if (address >= ramBase && address - ramBase <= size && bytes <= size - (address - ramBase))
            return static_cast<uint8_t *>(memory) + (address - ramBase);
        for (const auto &region : regions)
            if (address >= region.address && address - region.address <= region.size &&
                bytes <= region.size - (address - region.address))
                return static_cast<uint8_t *>(region.memory) + (address - region.address);
        throw std::runtime_error("HVF guest memory access outside mapped memory");
    }
};

bool HvfMachine::supported() {
    int value = 0;
    size_t size = sizeof(value);
    return sysctlbyname("kern.hv_support", &value, &size, nullptr, 0) == 0 && value;
}
HvfMachine::HvfMachine(size_t ramBytes, bool enableGic, SleepTicks sleepTicks, unsigned cpuCount)
    : HvfMachine(ramBytes,enableGic ? GicProfile::Native : GicProfile::Disabled,
                 std::move(sleepTicks),cpuCount) {}
HvfMachine::HvfMachine(size_t ramBytes, GicProfile profile, SleepTicks sleepTicks, unsigned cpuCount) : impl(std::make_unique<Impl>()) {
    impl->sleepTicks = std::move(sleepTicks);
    impl->initialize(ramBytes, profile!=GicProfile::Disabled, cpuCount, profile==GicProfile::HyperVNonsecure);
}
HvfMachine::~HvfMachine() = default;
std::span<uint8_t> HvfMachine::ram() {
    impl->requireOwner();
    return {static_cast<uint8_t *>(impl->memory), impl->size};
}
void HvfMachine::setSPI(uint32_t interrupt, bool level) {
    impl->requireOwner();
    if (!impl->gicEnabled) throw std::runtime_error("GICv3 is not enabled");
    if (__builtin_available(macOS 15.0, *)) check(hv_gic_set_spi(interrupt, level), "set GIC SPI");
    else throw std::runtime_error("GICv3 requires macOS 15");
}
void HvfMachine::mapROM(uint64_t address, std::span<const uint8_t> bytes) {
    impl->requireOwner();
    const auto hostPage = sysconf(_SC_PAGESIZE);
    if (hostPage <= 0) throw std::runtime_error("Cannot determine host page size");
    const auto page = size_t(hostPage);
    if (address % page || bytes.empty() || bytes.size() > 64 * 1024 * 1024)
        throw std::runtime_error("Invalid HVF ROM mapping");
    const auto size = (bytes.size() + page - 1) & ~(page - 1);
    if (address > UINT64_MAX - size ||
        (address < ramBase + impl->size && address + size > ramBase))
        throw std::runtime_error("ROM mapping overlaps RAM or overflows");
    for (const auto &region : impl->regions)
        if (address < region.address + region.size && address + size > region.address)
            throw std::runtime_error("ROM mapping overlaps existing mapping");
    void *memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED)
        throw std::runtime_error("Cannot allocate ROM mapping");
    std::memcpy(memory, bytes.data(), bytes.size());
    sys_icache_invalidate(memory, bytes.size());
    auto result = hv_vm_map(memory, address, size, HV_MEMORY_READ | HV_MEMORY_EXEC);
    if (result != HV_SUCCESS) {
        munmap(memory, size);
        check(result, "map ROM");
    }
    try { impl->regions.push_back({address, size, memory}); }
    catch (...) { hv_vm_unmap(address, size); munmap(memory, size); throw; }
}
void HvfMachine::writeMemory(uint64_t address, std::span<const uint8_t> bytes) {
    auto *destination = impl->range(address, bytes.size());
    if (!bytes.empty()) {
        std::memcpy(destination, bytes.data(), bytes.size());
        sys_icache_invalidate(destination, bytes.size());
    }
}
void HvfMachine::readMemory(uint64_t address, std::span<uint8_t> bytes) const {
    auto *source = impl->range(address, bytes.size());
    if (!bytes.empty())
        std::memcpy(bytes.data(), source, bytes.size());
}
void HvfMachine::start(uint64_t entry) {
    impl->range(entry, 4);
    if (entry & 3)
        throw std::runtime_error("ARM64 entry must be instruction aligned");
    impl->selected = 0;
    impl->primaryOnline = true;
    // ARM RES1 reset bits, no translation/data cache/instruction cache enabled.
    check(hv_vcpu_set_sys_reg(impl->vcpu, HV_SYS_REG_SCTLR_EL1, 0x30d00800), "set SCTLR_EL1");
    check(hv_vcpu_set_sys_reg(impl->vcpu, HV_SYS_REG_SP_EL1,
                             ramBase + impl->size), "set SP_EL1");
    check(hv_vcpu_set_reg(impl->vcpu, HV_REG_CPSR, 0x3c5), "set CPSR");
    setPC(entry);
}
unsigned HvfMachine::cpuCount() const { impl->requireOwner(); return 1 + unsigned(impl->secondary.size()); }
unsigned HvfMachine::currentCPU() const { impl->requireOwner(); return impl->selected; }
bool HvfMachine::cpuOnline(unsigned cpu) const {
    impl->requireOwner();
    if (cpu >= cpuCount()) throw std::runtime_error("Invalid HVF CPU index");
    return cpu == 0 ? impl->primaryOnline : impl->secondary[cpu - 1]->online;
}
int64_t HvfMachine::startCPU(unsigned cpu, uint64_t entry, uint64_t context) {
    impl->requireOwner();
    if (cpu >= cpuCount() || (entry & 3)) return -2;
    if (cpuOnline(cpu)) return -4;
    try { impl->range(entry, 4); } catch (const std::runtime_error &) { return -2; }
    if (cpu) impl->secondary[cpu - 1]->start(entry, context);
    else {
        auto previous = impl->selected; impl->selected = 0;
        start(entry);
        check(hv_vcpu_set_sys_reg(impl->vcpu, HV_SYS_REG_CNTV_CTL_EL0, 0), "reset primary timer");
        for (unsigned i = 0; i < 31; ++i) setReg(i, i == 0 ? context : 0);
        impl->primaryOnline = true; impl->selected = previous;
    }
    return 0;
}
int64_t HvfMachine::stopCPU() {
    impl->requireOwner();
    unsigned online = 0;
    for (unsigned i = 0; i < cpuCount(); ++i) online += cpuOnline(i);
    if (online <= 1) return -3; // Keep one CPU available to control the machine.
    if (impl->selected) impl->secondary[impl->selected - 1]->online = false;
    else impl->primaryOnline = false;
    return 0;
}
uint64_t HvfMachine::reg(unsigned index) const {
    impl->requireOwner();
    if (index > 30)
        throw std::runtime_error("ARM64 register index must be X0..X30");
    if (impl->selected) return impl->secondary[impl->selected - 1]->registers[index];
    uint64_t value;
    check(hv_vcpu_get_reg(impl->vcpu, static_cast<hv_reg_t>(HV_REG_X0 + index), &value), "get register");
    return value;
}
void HvfMachine::setReg(unsigned index, uint64_t value) {
    impl->requireOwner();
    if (index > 30)
        throw std::runtime_error("ARM64 register index must be X0..X30");
    if (impl->selected) { impl->secondary[impl->selected - 1]->setReg(index, value); return; }
    check(hv_vcpu_set_reg(impl->vcpu, static_cast<hv_reg_t>(HV_REG_X0 + index), value), "set register");
}
uint64_t HvfMachine::systemReg(uint16_t encoding) const {
    impl->requireOwner();
    if (impl->selected) return impl->secondary[impl->selected - 1]->systemReg(encoding);
    uint64_t value;
    check(hv_vcpu_get_sys_reg(impl->vcpu, static_cast<hv_sys_reg_t>(encoding), &value), "get system register");
    return value;
}
uint64_t HvfMachine::pc() const {
    impl->requireOwner();
    if (impl->selected) return impl->secondary[impl->selected - 1]->pc;
    uint64_t value;
    check(hv_vcpu_get_reg(impl->vcpu, HV_REG_PC, &value), "get PC");
    return value;
}
uint64_t HvfMachine::cpuPC(unsigned cpu) const {
    impl->requireOwner();
    if (cpu >= cpuCount()) throw std::runtime_error("Invalid HVF CPU index");
    if (cpu) return impl->secondary[cpu - 1]->pc;
    uint64_t value = 0;
    check(hv_vcpu_get_reg(impl->vcpu, HV_REG_PC, &value), "get primary PC");
    return value;
}
void HvfMachine::setPC(uint64_t value) {
    impl->requireOwner();
    if (value & 3)
        throw std::runtime_error("ARM64 PC must be instruction aligned");
    if (impl->selected) { impl->secondary[impl->selected - 1]->setPC(value); return; }
    check(hv_vcpu_set_reg(impl->vcpu, HV_REG_PC, value), "set PC");
}
HvfMachine::VirtualCounterSample HvfMachine::virtualCounterSample() const {
    impl->requireOwner();
    uint64_t offset=0;
    check(hv_vcpu_get_vtimer_offset(impl->vcpu, &offset), "sample virtual timer offset");
    return {mach_absolute_time()-offset, impl->clockPeriod, impl->sleepApplied};
}
HvfMachine::Exit HvfMachine::run(std::chrono::milliseconds budget) {
    impl->requireOwner();
    if (budget.count() <= 0 || budget > std::chrono::seconds(60))
        throw std::runtime_error("HVF run budget must be 1..60000 ms");
    auto takePending = [&] {
        auto exit = impl->pending.front(); impl->pending.pop_front();
        impl->selected = exit.cpu; return exit;
    };
    if (!impl->pending.empty()) return takePending();
    impl->synchronizeClock();
    impl->selected = 0;
    {
        std::lock_guard lock(impl->mutex);
        impl->active.clear();
        if (impl->primaryOnline) impl->active.push_back(impl->vcpu);
        for (auto &cpu : impl->secondary) if (cpu->online) impl->active.push_back(cpu->id);
        impl->deadline = std::chrono::steady_clock::now() + budget;
        impl->cancelResult = HV_SUCCESS;
        ++impl->generation;
        impl->armed = true;
        impl->changed.notify_one();
    }
    std::exception_ptr failure;
    Exit primary{ExitReason::Canceled, 0, 0, 0, 0};
    try {
        for (unsigned i = 0; i < impl->secondary.size(); ++i)
            if (impl->secondary[i]->online)
                impl->secondary[i]->beginRun(i + 1, [this, i](bool cancelOthers) {
                    impl->leaveCPU(impl->secondary[i]->id, cancelOthers);
                });
        if (impl->primaryOnline) {
            auto result = hv_vcpu_run(impl->vcpu);
            impl->leaveCPU(impl->vcpu, result != HV_SUCCESS || impl->exit->reason != HV_EXIT_REASON_CANCELED);
            check(result, "hv_vcpu_run"); ++impl->exitCount;
            auto reason = impl->exit->reason == HV_EXIT_REASON_EXCEPTION ? ExitReason::Exception :
                impl->exit->reason == HV_EXIT_REASON_CANCELED ? ExitReason::Canceled :
                impl->exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED ? ExitReason::VirtualTimer : ExitReason::Unknown;
            auto exception = reason == ExitReason::Exception ? impl->exit->exception : hv_vcpu_exit_exception_t{};
            primary = {reason, exception.syndrome, exception.virtual_address, exception.physical_address, pc()};
            primary.nativeReason = uint32_t(impl->exit->reason);
            if (reason == ExitReason::Unknown)
                primary.pstateAvailable = hv_vcpu_get_reg(impl->vcpu, HV_REG_CPSR, &primary.pstate) == HV_SUCCESS;
        }
    } catch (...) { failure = std::current_exception(); impl->cancelAll(); }
    // No guest CPU may access RAM while platform code emulates device DMA.
    for (auto &cpu : impl->secondary) if (cpu->online) {
        try { cpu->finish(); } catch (...) { if (!failure) failure = std::current_exception(); }
    }
    {
        std::lock_guard lock(impl->mutex);
        impl->armed = false;
        impl->changed.notify_one();
        if (impl->cancelResult != HV_SUCCESS && !failure)
            failure = std::make_exception_ptr(std::runtime_error("SMP vCPU cancellation failed"));
        impl->active.clear();
    }
    if (failure) std::rethrow_exception(failure);
    if (primary.reason != ExitReason::Canceled) impl->pending.push_back(primary);
    for (auto &cpu : impl->secondary)
        if (cpu->online && cpu->result.reason != ExitReason::Canceled) impl->pending.push_back(cpu->result);
    return impl->pending.empty() ? primary : takePending();
}

void HvfMachine::synchronizeInstructionContext(uint64_t cpuMask) { runMaintenance(cpuMask, false); }
void HvfMachine::flushTranslationContext(uint64_t cpuMask) { runMaintenance(cpuMask, true); }
void HvfMachine::runMaintenance(uint64_t cpuMask, bool flushTranslations) {
    impl->requireOwner();
    if (cpuMask & ~((1ull << cpuCount()) - 1))
        throw std::runtime_error("Invalid instruction synchronization CPU mask");
    if (!cpuMask) return;
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->active.empty() || impl->armed)
            throw std::runtime_error("Instruction synchronization requires stopped vCPUs");
    }
    // A read/execute-only stage-2 region outside platform RAM and MMIO. The
    // private entry executes no loads/stores and does not use guest registers.
    // Every EL1 vector catches unexpected exceptions instead of running the
    // guest's handler in this temporary context.
    constexpr uint64_t address = 0x3fff0000;
    if (!impl->contextRomMapped) {
        std::array<uint32_t, 1024> words;
        words.fill(0xd41fffa2); // HVC #0xfffd: unexpected EL1 exception.
        words[0] = 0xd5033f9f; // DSB SY
        words[1] = 0xd5033fdf; // ISB
        words[2] = 0xd41fffc2; // HVC #0xfffe: completed barrier.
        words[64] = 0xd5033f9f; // DSB SY
        words[65] = 0xd508831f; // TLBI VMALLE1IS: all stage-1 EL0/EL1 translations.
        words[66] = 0xd5033f9f; // DSB SY: wait for invalidation completion.
        words[67] = 0xd5033fdf; // ISB
        words[68] = 0xd41fffc2; // HVC #0xfffe: completed flush.
        mapROM(address, {reinterpret_cast<const uint8_t *>(words.data()), sizeof(words)});
        impl->contextRomMapped = true;
    }
    const uint64_t entry = address + (flushTranslations ? 0x100 : 0);
    const uint64_t completion = entry + (flushTranslations ? 20 : 12);
    auto execute = [&](hv_vcpu_t id, hv_vcpu_exit_t *exit) {
        constexpr std::array<hv_sys_reg_t, 7> names{
            HV_SYS_REG_SCTLR_EL1, HV_SYS_REG_VBAR_EL1, HV_SYS_REG_ELR_EL1,
            HV_SYS_REG_SPSR_EL1, HV_SYS_REG_ESR_EL1, HV_SYS_REG_FAR_EL1, HV_SYS_REG_MDSCR_EL1};
        std::array<uint64_t, names.size()> saved{};
        uint64_t pc = 0, cpsr = 0;
        bool timerMask = false;
        for (unsigned i = 0; i < names.size(); ++i)
            check(hv_vcpu_get_sys_reg(id, names[i], &saved[i]), "save barrier context");
        check(hv_vcpu_get_reg(id, HV_REG_PC, &pc), "save barrier PC");
        check(hv_vcpu_get_reg(id, HV_REG_CPSR, &cpsr), "save barrier PSTATE");
        check(hv_vcpu_get_vtimer_mask(id, &timerMask), "save barrier timer mask");
        auto restore = [&] {
            for (unsigned i = 0; i < names.size(); ++i)
                check(hv_vcpu_set_sys_reg(id, names[i], saved[i]), "restore barrier context");
            check(hv_vcpu_set_reg(id, HV_REG_PC, pc), "restore barrier PC");
            check(hv_vcpu_set_reg(id, HV_REG_CPSR, cpsr), "restore barrier PSTATE");
            check(hv_vcpu_set_vtimer_mask(id, timerMask), "restore barrier timer mask");
        };
        try {
            check(hv_vcpu_set_vtimer_mask(id, true), "mask barrier timer exit");
            check(hv_vcpu_set_sys_reg(id, HV_SYS_REG_MDSCR_EL1,
                saved[6] & ~uint64_t((1u << 15) | (1u << 13) | 1u)), "mask barrier debug exceptions");
            check(hv_vcpu_set_sys_reg(id, HV_SYS_REG_VBAR_EL1, address + 0x800), "set barrier vectors");
            check(hv_vcpu_set_sys_reg(id, HV_SYS_REG_SCTLR_EL1, saved[0] & ~uint64_t(1)), "set barrier physical execution");
            check(hv_vcpu_set_reg(id, HV_REG_CPSR, 0x3c5), "set barrier EL1h and DAIF");
            check(hv_vcpu_set_reg(id, HV_REG_PC, entry), "set barrier entry");
            for (;;) {
                check(hv_vcpu_run(id), "execute instruction barrier");
                if (exit->reason == HV_EXIT_REASON_CANCELED) {
                    if (std::chrono::steady_clock::now() >= impl->deadline)
                        throw std::runtime_error("Instruction synchronization timed out");
                    continue;
                }
                uint64_t completedPC = 0;
                check(hv_vcpu_get_reg(id, HV_REG_PC, &completedPC), "get barrier completion");
                if (exit->reason != HV_EXIT_REASON_EXCEPTION ||
                    (exit->exception.syndrome >> 26) != 0x16 ||
                    (exit->exception.syndrome & 0xffff) != 0xfffe || completedPC != completion)
                    throw std::runtime_error("Instruction synchronization did not complete");
                break;
            }
        } catch (...) { restore(); throw; }
        restore();
    };
    for (unsigned cpu = 0; cpu < cpuCount(); ++cpu) {
        if (!(cpuMask & (1ull << cpu)) || !cpuOnline(cpu)) continue;
        hv_vcpu_t id = cpu ? impl->secondary[cpu - 1]->id : impl->vcpu;
        // Reuse the persistent watchdog, including for WFI or malformed stub
        // failures. CPU lifetimes remain protected by the normal VM owner.
        auto disarm = [&] {
            std::lock_guard lock(impl->mutex);
            impl->armed = false; impl->active.clear(); impl->changed.notify_one();
        };
        {
            std::lock_guard lock(impl->mutex);
            impl->active = {id}; impl->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            impl->cancelResult = HV_SUCCESS; ++impl->generation; impl->armed = true;
            impl->changed.notify_one();
        }
        try {
            bool executed = true;
            if (cpu) executed = impl->secondary[cpu - 1]->withStoppedContext(execute);
            else execute(id, impl->exit);
            disarm();
            if (executed) {
                if (flushTranslations) ++impl->translationFlushes[cpu];
                else ++impl->contextBarriers[cpu];
            }
        } catch (...) { disarm(); throw; }
    }
}
uint64_t HvfMachine::instructionContextBarriers(unsigned cpu) const {
    impl->requireOwner();
    if (cpu >= cpuCount()) throw std::runtime_error("Invalid instruction barrier CPU index");
    return impl->contextBarriers[cpu];
}

uint64_t HvfMachine::translationContextFlushes(unsigned cpu) const {
    impl->requireOwner();
    if (cpu >= cpuCount()) throw std::runtime_error("Invalid translation flush CPU index");
    return impl->translationFlushes[cpu];
}

uint64_t HvfMachine::nativeGicDistributorRead(uint32_t offset) const {
    impl->requireOwner();
    {
        std::lock_guard lock(impl->mutex);
        if(!impl->gicEnabled || !impl->active.empty() || impl->armed || offset>=0x10000 || (offset&3))
            throw std::runtime_error("Native GIC access requires stopped VPs and an aligned register");
    }
    if(impl->hypervGicEnabled) {
        if(offset==8) return impl->gicIdentifiers[0];
        if(offset>=0xffd0) return impl->gicIdentifiers[1+(offset-0xffd0)/4];
    }
    uint64_t value=0;
    if(__builtin_available(macOS 15.0,*))
        check(hv_gic_get_distributor_reg(static_cast<hv_gic_distributor_reg_t>(offset),&value),"Read native distributor");
    else throw std::runtime_error("Native GIC access requires macOS 15");
    return value;
}
void HvfMachine::nativeGicDistributorWrite(uint32_t offset,uint64_t value) {
    impl->requireOwner();
    {
        std::lock_guard lock(impl->mutex);
        if(!impl->gicEnabled || !impl->active.empty() || impl->armed || offset>=0x10000 || (offset&3))
            throw std::runtime_error("Native GIC access requires stopped VPs and an aligned register");
    }
    if(__builtin_available(macOS 15.0,*))
        check(hv_gic_set_distributor_reg(static_cast<hv_gic_distributor_reg_t>(offset),value),"Write native distributor");
    else throw std::runtime_error("Native GIC access requires macOS 15");
}

void HvfMachine::sendSoftwareInterrupt(uint64_t cpuMask, unsigned interrupt) {
    if (interrupt >= 16) throw std::runtime_error("Invalid GIC software interrupt");
    sendPrivateInterrupt(cpuMask, interrupt);
    for (unsigned cpu = 0; cpu < cpuCount(); ++cpu) if (cpuMask & (1ull << cpu))
        ++impl->softwareInterrupts[cpu];
}

void HvfMachine::sendPrivateInterrupt(uint64_t cpuMask, unsigned interrupt) {
    impl->requireOwner();
    if (!impl->gicEnabled) throw std::runtime_error("GIC private interrupts require GICv3");
    if (interrupt >= 32 || (cpuMask & ~((1ull << cpuCount()) - 1)))
        throw std::runtime_error("Invalid GIC private interrupt or target mask");
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->active.empty() || impl->armed)
            throw std::runtime_error("GIC private interrupts require stopped vCPUs");
    }
    for (unsigned cpu = 0; cpu < cpuCount(); ++cpu) if (cpuMask & (1ull << cpu)) {
        if (cpu) impl->secondary[cpu - 1]->sendPrivateInterrupt(interrupt);
        else if (__builtin_available(macOS 15.0, *)) {
            uint64_t pending = 0;
            auto reg = static_cast<hv_gic_redistributor_reg_t>(0x10200); // GICR_ISPENDR0
            check(hv_gic_get_redistributor_reg(impl->vcpu, reg, &pending), "get primary pending private interrupts");
            check(hv_gic_set_redistributor_reg(impl->vcpu, reg, pending | (1ull << interrupt)), "pend primary private interrupt");
        } else throw std::runtime_error("GIC private interrupts require macOS 15");
        ++impl->privateInterrupts[cpu];
    }
}
HvfMachine::PrivateInterruptState HvfMachine::privateInterruptState(unsigned cpu) const {
    impl->requireOwner();
    if (cpu >= cpuCount()) throw std::runtime_error("Invalid private interrupt snapshot CPU index");
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->active.empty() || impl->armed)
            throw std::runtime_error("GIC snapshot requires stopped vCPUs");
    }
    PrivateInterruptState result;
    if (!impl->gicEnabled) return result;
    auto read = [&](hv_vcpu_t id, hv_vcpu_exit_t *) {
        if (__builtin_available(macOS 15.0, *)) {
            uint64_t enabled = 0, pending = 0, active = 0;
            check(hv_gic_get_redistributor_reg(id, static_cast<hv_gic_redistributor_reg_t>(0x10100), &enabled), "read GICR_ISENABLER0");
            check(hv_gic_get_redistributor_reg(id, static_cast<hv_gic_redistributor_reg_t>(0x10200), &pending), "read GICR_ISPENDR0");
            check(hv_gic_get_redistributor_reg(id, static_cast<hv_gic_redistributor_reg_t>(0x10300), &active), "read GICR_ISACTIVER0");
            result = {true, uint32_t(enabled), uint32_t(pending), uint32_t(active)};
        }
    };
    if (cpu) impl->secondary[cpu - 1]->withStoppedContext(read);
    else read(impl->vcpu, nullptr);
    return result;
}
uint64_t HvfMachine::privateInterruptRequests(unsigned cpu) const {
    impl->requireOwner();
    if (cpu >= cpuCount()) throw std::runtime_error("Invalid private interrupt CPU index");
    return impl->privateInterrupts[cpu];
}
uint64_t HvfMachine::softwareInterruptRequests(unsigned cpu) const {
    impl->requireOwner();
    if (cpu >= cpuCount()) throw std::runtime_error("Invalid software interrupt CPU index");
    return impl->softwareInterrupts[cpu];
}

uint64_t HvfMachine::exits() const {
    impl->requireOwner();
    auto result = impl->exitCount;
    for (auto &cpu : impl->secondary) result += cpu->exits;
    return result;
}
} // namespace pc
