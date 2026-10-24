#pragma once
#include <cstdint>
#include <deque>
#include <string>
namespace pc {
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
