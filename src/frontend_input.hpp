#pragma once
#include "frontend.hpp"
namespace pc {
// Backend-neutral input boundary. Coordinates use frontend conventions:
// relative counts or normalized absolute coordinates, Y down, wheel positive up,
// L/R/M buttons 1/2/4.
class GuestInput {
  public:
    virtual ~GuestInput() = default;
    virtual void key(uint16_t usage, bool released) = 0;
    virtual void pointer(int dx, int dy, int wheel, uint8_t buttons) = 0;
    virtual void absolutePointer(int, int, int, uint8_t) {}
    virtual void release() = 0;
    virtual void consumeCommand() = 0;
};
// One policy for every guest: capture click, host commands and focus loss.
class FrontendInput {
    Frontend *frontend;
    GuestInput &guest;
    bool captured = false;
    uint8_t buttons = 0;
    bool absolute = false;
    int absoluteX = 32768, absoluteY = 32768;

  public:
    FrontendInput(Frontend *ui, GuestInput &target) : frontend(ui), guest(target) {}
    void setAbsolutePointer(bool enabled) {
        if (absolute == enabled) return;
        release();
        absolute = enabled;
        if (frontend) frontend->setAbsolutePointer(enabled);
    }
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
            if (absolute) release();
            else if (captured)
                release();
            else
                captured = frontend && frontend->capturePointer(true);
            break;
        case K::Key:
            guest.key(e.key, e.released);
            break;
        case K::Button:
            if (!captured && !absolute) {
                if (!e.released && e.button == 1)
                    captured = frontend && frontend->capturePointer(true);
                break;
            }
            buttons = e.released ? buttons & ~e.button : buttons | e.button;
            if (absolute) guest.absolutePointer(absoluteX, absoluteY, 0, buttons);
            else guest.pointer(0, 0, 0, buttons);
            break;
        case K::AbsoluteMotion:
            if (absolute) {
                absoluteX = e.x; absoluteY = e.y;
                guest.absolutePointer(e.x, e.y, 0, buttons);
            }
            break;
        case K::PointerLeave:
            if (absolute && buttons) {
                buttons = 0;
                guest.absolutePointer(absoluteX, absoluteY, 0, 0);
            }
            break;
        case K::Motion:
            if (captured)
                guest.pointer(e.x, e.y, 0, buttons);
            break;
        case K::Wheel:
            if (absolute) guest.absolutePointer(absoluteX, absoluteY, e.y, buttons);
            else if (captured)
                guest.pointer(0, 0, e.y, buttons);
            break;
        default:
            break;
        }
        return true;
    }
};
} // namespace pc
