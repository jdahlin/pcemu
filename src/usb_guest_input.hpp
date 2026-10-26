#pragma once
#include "frontend_input.hpp"
#include "host_key_state.hpp"
#include "usb_xhci.hpp"
namespace pc {
class UsbGuestInput final : public GuestInput {
    UsbXhci &usb;
    HostKeyState keys{[this](uint16_t usage, bool up) { usb.receive_key_usage(usage, up); }};

  public:
    explicit UsbGuestInput(UsbXhci &controller) : usb(controller) {}
    void key(uint16_t usage, bool released) override {
        keys.event(usage, released);
    }
    void pointer(int dx, int dy, int wheel, uint8_t buttons) override {
        usb.receive_relative_motion(dx, dy, wheel, buttons);
    }
    void absolutePointer(int x, int y, int wheel, uint8_t buttons) override {
        usb.receive_absolute_motion(x, y, wheel, buttons);
    }
    void release() override {
        keys.release();
        usb.release_all_input();
    }
    void consumeCommand() override {
        keys.consumeCommand();
    }
};
} // namespace pc
