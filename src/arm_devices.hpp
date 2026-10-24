#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <span>
#include <utility>
#include <vector>
namespace pc {
enum class ArmPowerAction { None, Off, Reset };
struct ArmPsciResult { int64_t value; ArmPowerAction action = ArmPowerAction::None; };
// Single-CPU PSCI 0.2 subset. No power-down suspend or secondary CPUs.
ArmPsciResult armPsciCall(uint32_t function, uint64_t arg0, uint64_t arg1);
uint64_t armMmioLoad(uint64_t value, unsigned width, bool signExtend, bool wideRegister);
// Software OS-lock status/control for trapped EL1 debug-register accesses.
// Breakpoint/watchpoint execution remains owned by Hypervisor.framework.
class ArmOsLock {
  public:
    bool access(uint64_t syndrome, uint64_t &value);
  private:
    bool locked = true, doubleLocked = false;
};
// Two interleaved x16 Intel command-set NOR devices on a 32-bit bus.
// The firmware uses 256 KiB erase blocks and 128-byte write buffers.
class ArmNorFlash {
  public:
    explicit ArmNorFlash(std::span<const uint8_t> initial);
    void write(uint64_t offset, uint32_t value);
    std::span<const uint8_t> bytes() const { return visible; }
    std::span<const uint8_t> storage() const { return data; }
    std::pair<size_t, size_t> takeDirty();
  private:
    enum class Pending { None, Word, Erase, Lock, Count, Buffer, Confirm };
    Pending pending = Pending::None;
    std::vector<uint8_t> data, visible;
    std::vector<std::pair<size_t, uint32_t>> buffer;
    size_t statusLength = 4;
    size_t statusAddress = SIZE_MAX, blockAddress = 0, remaining = 0;
    size_t dirtyStart = SIZE_MAX, dirtyEnd = 0;
    void dirty(size_t offset, size_t length);
    void array();
    void status(size_t offset);
    void program(size_t offset, uint32_t value);
};
// PL011 console with level-triggered receive/transmit interrupts; no DMA.
class ArmPl011 {
  public:
    uint32_t read(unsigned offset);
    void write(unsigned offset, uint32_t value);
    void input(const std::string &text);
    const std::string &output() const { return transcript; }
    uint32_t rawInterrupts() const;
    bool irq() const { return (rawInterrupts() & interruptMask) != 0; }
  private:
    std::deque<uint8_t> receive;
    std::string transcript;
    uint32_t control = 0x300, integerDivisor = 0, fractionalDivisor = 0, lineControl = 0;
    uint32_t interruptMask = 0, fifoLevels = 0;
};
} // namespace pc
