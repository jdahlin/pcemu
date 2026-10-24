#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <vector>
namespace pc {
// Original ISA NE2000 / DP8390 implementation, fixed at 300h, IRQ10.
struct NE2000 {
    static constexpr uint16_t base = 0x300;
    std::array<uint8_t, 32768> memory{};
    std::array<uint8_t, 6> physical{0x02, 0x50, 0x43, 0x45, 0x4d, 0x01};
    std::array<uint8_t, 8> multicast{};
    std::array<uint8_t, 3> tally{};
    uint8_t command = 0x21, start = 0, stop = 0, boundary = 0, current = 0;
    uint8_t transmitPage = 0, transmitStatus = 0, receiveStatus = 0;
    uint8_t isr = 0x80, imr = 0, rcr = 0, tcr = 0, dcr = 0;
    uint16_t remote = 0, remoteCount = 0, transmitCount = 0, local = 0;
    uint64_t transmitted = 0, received = 0, dropped = 0;
    // A bounded wire queue is drained by the host outside the CPU hot loop.
    std::deque<std::vector<uint8_t>> outgoing;
    NE2000();
    void reset();
    bool irq() const {
        return (isr & imr & 0x7f) != 0;
    }
    uint32_t in(uint16_t port, int width);
    void out(uint16_t port, uint32_t value, int width);
    bool receive(const uint8_t *data, size_t length);

  private:
    uint8_t readMemory(uint16_t address) const;
    void writeMemory(uint16_t address, uint8_t value);
    uint8_t dmaRead();
    void dmaWrite(uint8_t value);
    void advanceDMA();
    void transmit();
};
} // namespace pc
