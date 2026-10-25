#pragma once
#include "host_key_state.hpp"
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <utility>
namespace pc {
class HostKeyboard {
    std::function<void(uint8_t)> destination;
    void emitBytes(std::initializer_list<uint8_t> bytes) { for (auto byte : bytes) destination(byte); }
    HostKeyState state{[this](uint16_t key, bool up) { emit(key, up); }};
    void emit(uint16_t key, bool up);

  public:
    explicit HostKeyboard(std::function<void(uint8_t)> sink) : destination(std::move(sink)) {}
    explicit HostKeyboard(std::deque<uint8_t> &queue)
        : HostKeyboard([&queue](uint8_t byte) { queue.push_back(byte); }) {}
    void event(uint16_t key, bool up) { state.event(key, up); }
    void release() { state.release(); }
    void consumeCommand() { state.consumeCommand(); }
};
} // namespace pc
