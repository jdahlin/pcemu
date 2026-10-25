#pragma once
#include "frontend_input.hpp"
#include "host_keyboard.hpp"
namespace pc {
// Translate neutral HID/axis events into PC scan codes and PS/2 coordinates.
// Callbacks keep the frontend adapter independent of CPU and device ownership.
class Ps2GuestInput final : public GuestInput {
    HostKeyboard keyboard;
    std::function<void(int, int, uint8_t, int)> motion;
    std::function<void()> wake;
    std::function<void(int,int,int,uint8_t)> absolute;
    int lastX = 32768, lastY = 32768;

  public:
    Ps2GuestInput(std::function<void(uint8_t)> scan,
                  std::function<void(int, int, uint8_t, int)> mouse, std::function<void()> resume,
                  std::function<void(int,int,int,uint8_t)> absoluteMotion = {})
        : keyboard(std::move(scan)), motion(std::move(mouse)), wake(std::move(resume)), absolute(std::move(absoluteMotion)) {}
    void key(uint16_t usage, bool released) override {
        keyboard.event(usage, released);
        wake();
    }
    void pointer(int dx, int dy, int wheel, uint8_t buttons) override {
        motion(dx, -dy, buttons, -wheel);
    }
    void absolutePointer(int x, int y, int wheel, uint8_t buttons) override {
        lastX = x; lastY = y;
        if (absolute) absolute(x, y, wheel, buttons);
    }
    void release() override {
        keyboard.release();
        motion(0, 0, 0, 0);
        if (absolute) absolute(lastX, lastY, 0, 0);
        wake();
    }
    void consumeCommand() override {
        keyboard.consumeCommand();
    }
};
} // namespace pc
