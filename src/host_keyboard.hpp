#pragma once
#include <SDL_scancode.h>
#include <array>
#include <cstdint>
#include <deque>
namespace pc {
class HostKeyboard {
    std::deque<uint8_t> &destination;
    std::array<bool, SDL_NUM_SCANCODES> held{}, sent{};
    void emit(SDL_Scancode key, bool up);

  public:
    explicit HostKeyboard(std::deque<uint8_t> &queue) : destination(queue) {}
    void event(SDL_Scancode key, bool up);
    void release();
    void consumeCommand();
};
} // namespace pc
