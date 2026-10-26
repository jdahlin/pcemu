#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace pc {
// ARM64 bare-metal platform, independent of the x86 Machine/CPU/JIT. One VM
// per process. Create, use and destroy on one dedicated thread; run() has a
// persistent watchdog that can request a cross-thread vCPU exit. Secondary CPUs
// have persistent owner threads; run() stops every CPU before returning to the
// caller for device emulation or RAM access.
class HvfMachine {
  public:
    static constexpr uint64_t ramBase = 0x40000000;
    enum class ExitReason { Exception, Canceled, VirtualTimer, Unknown };
    struct Exit {
        ExitReason reason;
        uint64_t syndrome, virtualAddress, physicalAddress, pc;
        unsigned cpu = 0;
        uint32_t nativeReason = UINT32_MAX;
        uint64_t pstate = 0;
        bool pstateAvailable = false;
        unsigned exceptionClass() const { return unsigned(syndrome >> 26) & 63; }
    };
    // Cumulative system-sleep time in mach_absolute_time ticks. The default
    // source samples mach_continuous_time; injection permits real vCPU tests
    // without putting the user's computer to sleep.
    using InitializationStage = std::function<void(const char *)>;
    using SleepTicks = std::function<uint64_t()>;
    explicit HvfMachine(size_t ramBytes = 2 * 1024 * 1024, bool enableGic = false,
                        SleepTicks sleepTicks = {}, unsigned cpuCount = 1, InitializationStage stage = {}, bool restoring = false);
    enum class GicProfile { Disabled, Native, HyperVNonsecure };
    HvfMachine(size_t ramBytes, GicProfile profile, SleepTicks sleepTicks = {},
               unsigned cpuCount = 1, InitializationStage stage = {}, bool restoring = false);
    ~HvfMachine();
    HvfMachine(const HvfMachine &) = delete;
    HvfMachine &operator=(const HvfMachine &) = delete;
    std::span<uint8_t> ram();
    void setSPI(uint32_t interrupt, bool level);
    static constexpr uint64_t msiFrameBase = 0x08020000, msiSetSpi = msiFrameBase + 0x40;
    static constexpr uint32_t msiBase = 64, msiCount = 64;
    void sendMSI(uint64_t address, uint32_t interrupt);
    // Backend for the Hyper-V nonsecure distributor view, all VPs stopped.
    uint64_t nativeGicDistributorRead(uint32_t offset) const;
    void nativeGicDistributorWrite(uint32_t offset, uint64_t value);
    void mapROM(uint64_t address, std::span<const uint8_t> bytes);
    void writeMemory(uint64_t address, std::span<const uint8_t> bytes);
    void readMemory(uint64_t address, std::span<uint8_t> bytes) const;
    // Initialize a flat EL1h AArch64 guest, MMU off, interrupts masked.
    void start(uint64_t entry);
    unsigned cpuCount() const;
    unsigned currentCPU() const;
    bool cpuOnline(unsigned cpu) const;
    int64_t startCPU(unsigned cpu, uint64_t entry, uint64_t context);
    int64_t stopCPU();
    uint64_t reg(unsigned index) const; // X0..X30
    void setReg(unsigned index, uint64_t value);
    uint64_t systemReg(uint16_t encoding) const;
    uint64_t pc() const;
    uint64_t cpuPC(unsigned cpu) const;
    void setPC(uint64_t value);
    Exit run(std::chrono::milliseconds budget = std::chrono::milliseconds(1000));
    // Thread-safe and latched: a readiness notification just before run()
    // must not be lost between device service and arming the guest CPUs.
    void requestExit() noexcept;
    // All requested online vCPUs execute DSB SY; ISB while the normal guest
    // context is preserved. Throws on incomplete execution; never acknowledges
    // a barrier based solely on stopping a host thread.
    void synchronizeInstructionContext(uint64_t cpuMask);
    uint64_t instructionContextBarriers(unsigned cpu) const;
    // Conservative full EL1 translation invalidation, including all ASIDs and
    // global mappings, on every selected online VP before returning.
    void flushTranslationContext(uint64_t cpuMask);
    uint64_t translationContextFlushes(unsigned cpu) const;
    // Pend a GIC software-generated interrupt (SGI 0..15), preserving other
    // pending bits. Delivery/coalescing follows the guest's GIC configuration.
    void sendSoftwareInterrupt(uint64_t cpuMask, unsigned interrupt);
    uint64_t softwareInterruptRequests(unsigned cpu) const;
    // Edge-pend an SGI or PPI (INTID 0..31) on selected VPs. This does not
    // model an asserted level source or replace the architectural timer.
    void sendPrivateInterrupt(uint64_t cpuMask, unsigned interrupt);
    uint64_t privateInterruptRequests(unsigned cpu) const;
    struct PrivateInterruptState {
        bool available = false;
        uint32_t enabled = 0, pending = 0, active = 0;
    };
    // Read-only GIC snapshot; all VPs must be stopped. Offline VPs are unavailable.
    PrivateInterruptState privateInterruptState(unsigned cpu) const;
    struct VirtualCounterSample {
        uint64_t value, frequency, sleepAdjustment;
    };
    // Paired with the offset used by CNTVCT on every VP. All VPs stopped.
    VirtualCounterSample virtualCounterSample() const;
    // Called with all VPs stopped, before and after a host-sleep offset change.
    // A failed backend update leaves consumers invalidated; callbacks may throw.
    void setCounterRebaseHandler(std::function<void(bool before)> handler);
    // Host lifecycle only, all VPs stopped. Freeze virtual time during pause.
    void pauseClock(bool paused);
    // Native GIC, VP register/timer and pending-exit state. Restore into a fresh
    // restoring=true VM before any VP runs; RAM/devices are owned by the platform.
    std::vector<uint8_t> saveState();
    void restoreState(std::span<const uint8_t> bytes);
    uint64_t exits() const;
    static bool supported();

  private:
    void runMaintenance(uint64_t cpuMask, bool flushTranslations);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace pc
