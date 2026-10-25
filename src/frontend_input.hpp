#pragma once
#include "frontend.hpp"
namespace pc {
// Backend-neutral input boundary. Coordinates use frontend conventions:
// relative pixels, positive Y down, wheel positive up, L/R/M buttons 1/2/4.
class GuestInput {
  public:
    virtual ~GuestInput() = default;
    virtual void key(uint16_t usage, bool released) = 0;
    virtual void pointer(int dx, int dy, int wheel, uint8_t buttons) = 0;
    virtual void release() = 0;
    virtual void consumeCommand() = 0;
};
// One policy for every guest: capture click, host commands and focus loss.
class FrontendInput {
    Frontend *frontend;
    GuestInput &guest;
    bool captured = false;
    uint8_t buttons = 0;

  public:
    FrontendInput(Frontend *ui, GuestInput &target) : frontend(ui), guest(target) {}
    void release() {
        guest.release();
        buttons = 0;
        captured = false;
        if (frontend)
            frontend->capturePointer(false);
    }
    // False requests shutdown. Host focus/quit events apply even with input off.
    bool event(const FrontendEvent &e, bool enabled = true) {
        using K = FrontendEvent::Kind;
        if (e.kind == K::Quit) {
            release();
            return false;
        }
        if (e.kind == K::FocusLost) {
            release();
            return true;
        }
        if (!enabled)
            return true;
        switch (e.kind) {
        case K::ToggleCapture:
            guest.consumeCommand();
            if (captured)
                release();
            else
                captured = frontend && frontend->capturePointer(true);
            break;
        case K::Key:
            guest.key(e.key, e.released);
            break;
        case K::Button:
            if (!captured) {
                if (!e.released && e.button == 1)
                    captured = frontend && frontend->capturePointer(true);
                break;
            }
            buttons = e.released ? buttons & ~e.button : buttons | e.button;
            guest.pointer(0, 0, 0, buttons);
            break;
        case K::Motion:
            if (captured)
                guest.pointer(e.x, e.y, 0, buttons);
            break;
        case K::Wheel:
            if (captured)
                guest.pointer(0, 0, e.y, buttons);
            break;
        default:
            break;
        }
        return true;
    }
};
} // namespace pc
