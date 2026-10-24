#pragma once
#include <array>
#include <cstdint>
namespace pc {
// One PCI display function, configuration mechanism 1. The compatible ID
// selects a guest driver for our original DISPI implementation.
struct PCI {
    uint32_t address = 0;
    std::array<uint8_t, 256> config{};
    bool barProbe = false, romProbe = false;
    PCI();
    uint32_t read(uint16_t bdf, unsigned offset, unsigned width) const;
    void write(uint16_t bdf, unsigned offset, uint32_t value, unsigned width);
    uint32_t in(uint16_t port, unsigned width) const;
    void out(uint16_t port, uint32_t value, unsigned width);
};
} // namespace pc
