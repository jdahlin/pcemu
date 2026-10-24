#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <utility>
namespace pc {
class HostKeyboard {
    std::function<void(uint8_t)> destination;
    void emitBytes(std::initializer_list<uint8_t> bytes) { for (auto byte : bytes) destination(byte); }
    std::array<bool, 512> held{}, sent{};
    void emit(uint16_t key, bool up);

  public:
    explicit HostKeyboard(std::function<void(uint8_t)> sink) : destination(std::move(sink)) {}
    explicit HostKeyboard(std::deque<uint8_t> &queue)
        : HostKeyboard([&queue](uint8_t byte) { queue.push_back(byte); }) {}
    void event(uint16_t key, bool up);
    void release();
    void consumeCommand();
};
} // namespace pc
