#include "hvf_machine.hpp"
#include "hvf_secondary.hpp"
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
    void initialize(size_t bytes, bool enableGic, unsigned cpuCount) {
        if (!cpuCount || cpuCount > 8 || (cpuCount > 1 && !enableGic))
            throw std::runtime_error("HVF supports 1..8 CPUs; SMP requires GICv3");
        auto pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0 || bytes < size_t(pageSize) || bytes > size_t(2) * 1024 * 1024 * 1024 ||
            bytes % size_t(pageSize))
            throw std::runtime_error("HVF RAM must be host-page aligned and between one page and 2 GiB");
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
                auto result = hv_gic_config_set_distributor_base(config, 0x08000000);
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
            uint64_t features = 0;
            check(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, &features), "get PMU features");
            check(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1,
                                      (features & ~0xf00ull) | 0x100), "enable framework PMUv3");
        }
        uint64_t timerOffset = 0;
        check(hv_vcpu_get_vtimer_offset(vcpu, &timerOffset), "get initial virtual timer offset");
        for (unsigned i = 1; i < cpuCount; ++i)
            secondary.push_back(std::make_unique<HvfSecondary>(i, timerOffset, ramBase + size));
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
HvfMachine::HvfMachine(size_t ramBytes, bool enableGic, SleepTicks sleepTicks, unsigned cpuCount) : impl(std::make_unique<Impl>()) {
    impl->sleepTicks = std::move(sleepTicks);
    impl->initialize(ramBytes, enableGic, cpuCount);
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

uint64_t HvfMachine::exits() const {
    impl->requireOwner();
    auto result = impl->exitCount;
    for (auto &cpu : impl->secondary) result += cpu->exits;
    return result;
}
} // namespace pc
