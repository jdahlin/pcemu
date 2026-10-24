#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pc {
// ARM64 bare-metal platform, independent of the x86 Machine/CPU/JIT. One VM
// per process. Create, use and destroy on one dedicated thread; run() has a
// persistent watchdog that can request a cross-thread vCPU exit.
class HvfMachine {
  public:
    static constexpr uint64_t ramBase = 0x40000000;
    enum class ExitReason { Exception, Canceled, VirtualTimer, Unknown };
    struct Exit {
        ExitReason reason;
        uint64_t syndrome, virtualAddress, physicalAddress, pc;
        unsigned exceptionClass() const { return unsigned(syndrome >> 26) & 63; }
    };
    explicit HvfMachine(size_t ramBytes = 2 * 1024 * 1024);
    ~HvfMachine();
    HvfMachine(const HvfMachine &) = delete;
    HvfMachine &operator=(const HvfMachine &) = delete;
    void writeMemory(uint64_t address, std::span<const uint8_t> bytes);
    void readMemory(uint64_t address, std::span<uint8_t> bytes) const;
    // Initialize a flat EL1h AArch64 guest, MMU off, interrupts masked.
    void start(uint64_t entry);
    uint64_t reg(unsigned index) const; // X0..X30
    void setReg(unsigned index, uint64_t value);
    uint64_t pc() const;
    void setPC(uint64_t value);
    Exit run(std::chrono::milliseconds budget = std::chrono::milliseconds(1000));
    uint64_t exits() const;
    static bool supported();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace pc
