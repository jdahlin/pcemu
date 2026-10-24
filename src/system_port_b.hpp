#pragma once
#include <cstdint>
namespace pc {
// PC system control port B (61h): a stored output latch plus PIT output pins.
// The board connects the returned gate level to PIT channel 2.
class SystemPortBDevice {
  public:
    void reset() { latch_ = 0; }
    void restore(uint8_t value) { latch_ = value; }
    uint8_t read(bool timer1, bool timer2) const {
        return (latch_ & 0xcf) | (timer1 ? 16 : 0) | (timer2 ? 32 : 0);
    }
    bool write(uint8_t value) { latch_ = value; return value & 1; }
  private:
    uint8_t latch_ = 0;
};
} // namespace pc
