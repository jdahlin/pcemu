#include "frontend_input.hpp"
#include "host_key_state.hpp"
#include "ps2_guest_input.hpp"
#include <cassert>
#include <iostream>
using namespace pc;
struct UI : Frontend {
    bool captured = false, allow = true;
    std::vector<FrontendEvent> poll() override {
        return {};
    }
    void present(std::span<const uint32_t>, unsigned, unsigned, DisplayShape) override {}
    bool capturePointer(bool enable) override {
        captured = enable && allow;
        return !enable || allow;
    }
};
struct Input : GuestInput {
    std::vector<FrontendEvent> events;
    HostKeyState keys{
        [this](uint16_t k, bool up) { events.push_back({FrontendEvent::Kind::Key, k, up}); }};
    void key(uint16_t k, bool up) override {
        keys.event(k, up);
    }
    void pointer(int x, int y, int wheel, uint8_t b) override {
        events.push_back({FrontendEvent::Kind::Motion, 0, false, b, x, y});
        events.back().key = wheel;
    }
    void release() override {
        keys.release();
        pointer(0, 0, 0, 0);
    }
    void consumeCommand() override {
        keys.consumeCommand();
    }
};
int main() {
    using K = FrontendEvent::Kind;
    UI ui;
    Input guest;
    FrontendInput input(&ui, guest);
    input.event({K::Key, hid::KEY_LGUI});
    input.event({K::ToggleCapture});
    input.event({K::Key, hid::KEY_LGUI, true});
    assert(guest.events.empty());
    assert(ui.captured);
    input.event({K::Key, hid::KEY_A});
    input.event({K::Button, 0, false, 2});
    input.event({K::Motion, 0, false, 0, 4, -8});
    input.event({K::Wheel, 0, false, 0, 0, 2});
    assert(guest.events[2].x == 4 && guest.events[2].y == -8 && guest.events[2].button == 2);
    assert(guest.events[3].key == 2);
    input.event({K::FocusLost}, false);
    assert(!ui.captured);
    assert(guest.events[4].key == hid::KEY_A && guest.events[4].released);
    assert(guest.events.back().button == 0);
    auto count = guest.events.size();
    input.event({K::Motion, 0, false, 0, 2, 3});
    assert(guest.events.size() == count);
    ui.allow = false;
    input.event({K::Button, 0, false, 1});
    assert(!ui.captured);
    input.event({K::Key, hid::KEY_B}, false);
    assert(guest.events.size() == count);
    assert(!input.event({K::Quit}, false));
    std::vector<uint8_t> scans;
    int dx = 0, dy = 0, wh = 0;
    unsigned buttons = 0, wakes = 0;
    Ps2GuestInput ps2([&](uint8_t b) { scans.push_back(b); },
                      [&](int x, int y, uint8_t b, int w) {
                          dx = x;
                          dy = y;
                          buttons = b;
                          wh = w;
                      },
                      [&] { ++wakes; });
    UI ui2;
    FrontendInput x86(&ui2, ps2);
    x86.event({K::Key, hid::KEY_A});
    x86.event({K::Key, hid::KEY_A, true});
    assert((scans == std::vector<uint8_t>{0x1e, 0x9e}));
    x86.event({K::Button, 0, false, 1}); // capture click is consumed
    x86.event({K::Button, 0, false, 1});
    x86.event({K::Motion, 0, false, 0, 4, -8});
    assert(dx == 4 && dy == 8 && buttons == 1);
    x86.event({K::Wheel, 0, false, 0, 0, 2});
    assert(wh == -2);
    x86.event({K::FocusLost});
    assert(!ui2.captured && buttons == 0 && wakes == 3);
    // Bare Command is a Windows key; host shortcuts consume it without a tap.
    scans.clear();
    ps2.key(hid::KEY_LGUI, false);
    ps2.key(hid::KEY_LGUI, true);
    assert((scans == std::vector<uint8_t>{0xe0, 0x5b, 0xe0, 0xdb}));
    std::cout << "Shared input capture, shortcuts, focus, disable and PS/2 conversion passed\n";
}
