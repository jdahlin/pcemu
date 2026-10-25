#pragma once
// Internal HVF worker. Every vCPU API runs on its owning persistent thread.
#include "hvf_machine.hpp"
#include <Hypervisor/Hypervisor.h>
#include <array>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace pc {
class HvfSecondary {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread thread;
    std::function<void()> job;
    std::exception_ptr failure;
    bool ready = false, busy = false, stopping = false;
    bool needsStart = false, pcDirty = false, offsetDirty = false;
    uint64_t entry = 0, context = 0, stack = 0, offset = 0;
    std::array<bool, 31> dirty{};
    static void check(hv_return_t result) {
        if (result != HV_SUCCESS) throw std::runtime_error("Secondary HVF vCPU operation failed: " + std::to_string(result));
    }
    void initializeRegisters() {
        check(hv_vcpu_set_sys_reg(id, HV_SYS_REG_SCTLR_EL1, 0x30d00800));
        check(hv_vcpu_set_sys_reg(id, HV_SYS_REG_SP_EL1, stack));
        check(hv_vcpu_set_sys_reg(id, HV_SYS_REG_CNTV_CTL_EL0, 0));
        check(hv_vcpu_set_reg(id, HV_REG_CPSR, 0x3c5));
        check(hv_vcpu_set_reg(id, HV_REG_PC, entry));
        for (unsigned i = 0; i < 31; ++i)
            check(hv_vcpu_set_reg(id, static_cast<hv_reg_t>(HV_REG_X0 + i), i == 0 ? context : 0));
        dirty.fill(false); pcDirty = false; needsStart = false;
    }
  public:
    hv_vcpu_t id = 0;
    hv_vcpu_exit_t *exit = nullptr;
    std::array<uint64_t, 31> registers{};
    uint64_t pc = 0, exits = 0;
    bool online = false;
    HvfMachine::Exit result{HvfMachine::ExitReason::Canceled, 0, 0, 0, 0};
    HvfSecondary(unsigned index, uint64_t timerOffset, uint64_t initialStack) : stack(initialStack) {
        thread = std::thread([this, index, timerOffset] {
            bool created = false;
            try {
                check(hv_vcpu_create(&id, &exit, nullptr)); created = true;
                check(hv_vcpu_set_sys_reg(id, HV_SYS_REG_MPIDR_EL1, 0x80000000ull | index));
                check(hv_vcpu_set_vtimer_offset(id, timerOffset));
                uint64_t features = 0;
                check(hv_vcpu_get_sys_reg(id, HV_SYS_REG_ID_AA64DFR0_EL1, &features));
                check(hv_vcpu_set_sys_reg(id, HV_SYS_REG_ID_AA64DFR0_EL1, (features & ~0xf00ull) | 0x100));
            } catch (...) { failure = std::current_exception(); }
            std::unique_lock lock(mutex);
            ready = true; changed.notify_all();
            while (!stopping) {
                changed.wait(lock, [&] { return stopping || busy; });
                if (stopping) break;
                auto action = std::move(job);
                lock.unlock();
                try { action(); } catch (...) { failure = std::current_exception(); }
                lock.lock(); busy = false; changed.notify_all();
            }
            lock.unlock();
            if (created) hv_vcpu_destroy(id);
        });
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return ready; });
        if (failure) {
            auto error = failure; stopping = true; changed.notify_all(); lock.unlock();
            thread.join(); std::rethrow_exception(error);
        }
    }
    ~HvfSecondary() {
        { std::lock_guard lock(mutex); stopping = true; changed.notify_all(); }
        thread.join();
    }
    void dispatch(std::function<void()> action) {
        std::lock_guard lock(mutex);
        if (busy) throw std::runtime_error("Secondary vCPU is already running");
        job = std::move(action); busy = true; changed.notify_all();
    }
    void finish() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return !busy; });
        if (failure) std::rethrow_exception(failure);
    }
    void start(uint64_t address, uint64_t value) {
        online = true; needsStart = true; entry = address; context = value;
    }
    void setReg(unsigned index, uint64_t value) { registers[index] = value; dirty[index] = true; }
    void setPC(uint64_t value) { pc = value; pcDirty = true; }
    void setOffset(uint64_t value) { offset = value; offsetDirty = true; }
    uint64_t systemReg(uint16_t encoding) {
        uint64_t value = 0;
        dispatch([&] { check(hv_vcpu_get_sys_reg(id, static_cast<hv_sys_reg_t>(encoding), &value)); });
        finish(); return value;
    }
    bool withStoppedContext(const std::function<void(hv_vcpu_t, hv_vcpu_exit_t *)> &action) {
        // A never-started or powered-off VP has no old instruction stream.
        if (!online || needsStart) return false;
        std::exception_ptr error;
        dispatch([&] { try { action(id, exit); } catch (...) { error = std::current_exception(); } });
        finish();
        if (error) std::rethrow_exception(error);
        return true;
    }
    void sendPrivateInterrupt(unsigned interrupt) {
        dispatch([&] {
            if (__builtin_available(macOS 15.0, *)) {
                uint64_t pending = 0;
                auto reg = static_cast<hv_gic_redistributor_reg_t>(0x10200); // GICR_ISPENDR0
                check(hv_gic_get_redistributor_reg(id, reg, &pending));
                check(hv_gic_set_redistributor_reg(id, reg, pending | (1ull << interrupt)));
            } else throw std::runtime_error("GIC private interrupts require macOS 15");
        });
        finish();
    }
    void beginRun(unsigned index, std::function<void(bool)> leave) {
        dispatch([this, index, leave = std::move(leave)] {
            try {
                if (needsStart) initializeRegisters();
                if (offsetDirty) { check(hv_vcpu_set_vtimer_offset(id, offset)); offsetDirty = false; }
                for (unsigned i = 0; i < 31; ++i) if (dirty[i]) {
                    check(hv_vcpu_set_reg(id, static_cast<hv_reg_t>(HV_REG_X0 + i), registers[i])); dirty[i] = false;
                }
                if (pcDirty) { check(hv_vcpu_set_reg(id, HV_REG_PC, pc)); pcDirty = false; }
                auto status = hv_vcpu_run(id);
                leave(status != HV_SUCCESS || exit->reason != HV_EXIT_REASON_CANCELED);
                check(status); ++exits;
                auto reason = exit->reason == HV_EXIT_REASON_EXCEPTION ? HvfMachine::ExitReason::Exception :
                    exit->reason == HV_EXIT_REASON_CANCELED ? HvfMachine::ExitReason::Canceled :
                    exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED ? HvfMachine::ExitReason::VirtualTimer : HvfMachine::ExitReason::Unknown;
                check(hv_vcpu_get_reg(id, HV_REG_PC, &pc));
                if (reason != HvfMachine::ExitReason::Canceled)
                    for (unsigned i = 0; i < 31; ++i)
                        check(hv_vcpu_get_reg(id, static_cast<hv_reg_t>(HV_REG_X0 + i), &registers[i]));
                auto exception = reason == HvfMachine::ExitReason::Exception ? exit->exception : hv_vcpu_exit_exception_t{};
                result = {reason, exception.syndrome, exception.virtual_address, exception.physical_address, pc, index};
            } catch (...) { leave(true); throw; }
        });
    }
};
} // namespace pc
