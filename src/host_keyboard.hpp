#pragma once
#include <array>
#include <cstdint>
#include <deque>
namespace pc {
class HostKeyboard {
    std::deque<uint8_t> &destination;
    std::array<bool, 512> held{}, sent{};
    void emit(uint16_t key, bool up);

  public:
    explicit HostKeyboard(std::deque<uint8_t> &queue) : destination(queue) {}
    void event(uint16_t key, bool up);
    void release();
    void consumeCommand();
};
} // namespace pc
