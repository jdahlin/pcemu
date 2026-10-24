#pragma once
#include <cstdint>
namespace pc {
// Fast A20/reset port. A20 is an external pin sampled at read time; writes
// describe signal changes without accessing the CPU or the machine lifecycle.
class Port92Device {
  public:
    struct Effects { bool a20, reset; };
    uint8_t read(bool a20) const { return a20 ? 2 : 0; }
    Effects write(uint8_t value) const { return {bool(value & 2), bool(value & 1)}; }
};
} // namespace pc
