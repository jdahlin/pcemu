#pragma once
#include <array>
#include <cstdint>
#include <deque>
namespace pc {
struct PS2Mouse {
    bool enabled = false, remote = false, wrap = false, scaling2 = false;
    uint8_t resolution = 2, rate = 100, id = 0, parameter = 0, buttons = 0, reportedButtons = 0,
            lastSent = 0;
    std::array<uint8_t, 3> rates{};
    int32_t dx = 0, dy = 0, wheel = 0;
    uint64_t nextAt = 0;
    std::deque<uint8_t> replies, packets;
    void command(uint8_t value);
    void motion(int x, int y, uint8_t held, int scroll = 0);
    void poll(uint64_t instructions);
    void packet(std::deque<uint8_t> &destination);
    uint8_t status() const;
    void defaults();
};
} // namespace pc
