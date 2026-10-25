#include "frontend.hpp"
#include "host_key.hpp"
#include "win95_shutdown.hpp"
#include <SDL.h>
#include <iostream>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        std::vector<uint8_t> ram(1024, 0);
        const uint8_t terminal[] = {0xb3, 3, 0xb8, 0x0e, 0x16, 0xcd, 0x2f, 0x90, 0xeb, 0xfd};
        std::copy(std::begin(terminal), std::end(terminal), ram.begin() + 100);
        check(win95ShutdownComplete(ram, 0x10, 80, 27, 0x160e, 3), "WIN.COM safe loop recognized");
        check(win95ShutdownComplete(ram, 0x10, 80, 28, 0x160e, 3), "both safe-loop instructions recognized");
        check(!win95ShutdownComplete(ram, 0x11, 80, 27, 0x160e, 3), "protected mode is not shutdown");
        check(!win95ShutdownComplete(ram, 0x10, 80, 27, 0, 3), "shutdown broadcast required");
        check(!win95ShutdownComplete(ram, 0x10, 80, 27, 0x160e, 0), "shutdown broadcast subtype required");
        check(!win95ShutdownComplete(ram, 0x10, 80, 29, 0x160e, 3), "unrelated guest code is not shutdown");
        check(!win95ShutdownComplete(ram, 0x10, 100000, 27, 0x160e, 3), "invalid guest address rejected");
        ram[100] = 0;
        check(!win95ShutdownComplete(ram, 0x10, 80, 27, 0x160e, 3), "exact shutdown code required");
        Win95ShutdownRequest shutdown;
        std::vector<unsigned> keys, scans;
        auto tick = [&](uint64_t t, uint64_t count, bool mode = true, bool empty = true) {
            shutdown.tick(t, count, mode, empty,
                          [&](uint8_t s) { scans.push_back(s); },
                          [&](uint16_t k) { keys.push_back(k); });
        };
        tick(0, 0); check(scans.empty(), "no input before close request");
        shutdown.request(0, 0);
        tick(0, 0, false); check(scans.empty(), "wait for Windows protected mode");
        tick(0, 0, true, false); check(scans.empty(), "wait for keyboard queue to drain");
        tick(0, 0); tick(1, 1); check(scans.size() == 1, "keys paced by time and guest progress");
        shutdown.request(0, 0); // Repeated close must not restart or cut power.
        for (unsigned i = 1; i < 50; ++i) tick(i * 1000, i * 1000000);
        check(scans == std::vector<unsigned>{0x1d, 1, 0x81, 0x9d}, "Ctrl+Escape make and break order");
        std::string command;
        for (auto k : keys) if (k < 256) command += char(k);
        check(command == "rrundll32.exe user.exe,ExitWindows", "normal guest shutdown command");
        check(keys.back() == 0x1c0d, "Run command submitted once");
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SCALING, "1");
        auto frontend = createFrontend(640, 400, DisplayShape::ClassicVga);
        check(!SDL_GetHintBoolean(SDL_HINT_MOUSE_RELATIVE_SCALING, SDL_TRUE),
              "relative mouse counts stay independent of logical display scaling");
        auto *window = SDL_GetWindowFromID(1);
        check(window != nullptr, "headless window exists");
        auto *renderer = SDL_GetRenderer(window);
        int windowWidth, windowHeight;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        check(windowWidth * 3 == windowHeight * 4, "classic VGA window starts at 4:3");
        std::vector<uint32_t> modeFrame;
        for (auto [w, h] : {std::pair{640u, 400u}, {320u, 200u}, {320u, 400u},
                            {640u, 350u}, {640u, 480u}}) {
            modeFrame.assign(w * h, 0xff123456);
            frontend->present(modeFrame, w, h, DisplayShape::ClassicVga);
            int logicalWidth, logicalHeight;
            SDL_RenderGetLogicalSize(renderer, &logicalWidth, &logicalHeight);
            check(logicalWidth * 3 == logicalHeight * 4, "classic mode retains 4:3 output");
            int currentWidth, currentHeight;
            SDL_GetWindowSize(window, &currentWidth, &currentHeight);
            check(currentWidth == windowWidth && currentHeight == windowHeight,
                  "mode switches keep window size stable");
        }
        // Same texture size: switching policy must still update presentation.
        modeFrame.assign(640 * 400, 0xff123456);
        frontend->present(modeFrame, 640, 400, DisplayShape::ClassicVga);
        frontend->present(modeFrame, 640, 400);
        int logicalWidth, logicalHeight;
        SDL_RenderGetLogicalSize(renderer, &logicalWidth, &logicalHeight);
        check(logicalWidth == 640 && logicalHeight == 400,
              "VBE and ARM default to square pixels even at a classic resolution");
        frontend->present(modeFrame, 640, 400, DisplayShape::ClassicVga);
        SDL_RenderGetLogicalSize(renderer, &logicalWidth, &logicalHeight);
        check(logicalWidth == 4 && logicalHeight == 3, "same-size return to VGA restores aspect");
        frontend->poll();
        SDL_Event motion{};
        motion.type = SDL_MOUSEMOTION;
        motion.motion.windowID = SDL_GetWindowID(window);
        motion.motion.xrel = 120;
        motion.motion.yrel = -75;
        SDL_PushEvent(&motion);
        auto movement = frontend->poll();
        check(movement.size() == 1 && movement[0].kind == FrontendEvent::Kind::Motion &&
                  movement[0].x == 120 && movement[0].y == -75,
              "classic VGA presentation preserves relative movement counts");
        std::vector<uint32_t> frame(640 * 400, 0xff123456);
        frontend->present(frame, 640, 400);
        frame.resize(1024 * 768, 0xffabcdef);
        frontend->present(frame, 1024, 768);
        SDL_SetWindowSize(window, 320, 240);
        frame.assign(63 * 47, 0xffffffff);
        frontend->present(frame, 63, 47);
        check(SDL_RenderGetIntegerScale(renderer), "SVGA uses whole physical pixel scaling");
        float sx = 0, sy = 0;
        SDL_RenderGetScale(renderer, &sx, &sy);
        check(sx == 5 && sy == 5, "SVGA pixels remain square at integer scale");
        std::vector<uint32_t> output(320 * 240);
        check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                   output.data(), 320 * 4) == 0, "headless output readable");
        unsigned white = 0, other = 0;
        for (auto pixel : output) {
            white += (pixel & 0xffffff) == 0xffffff;
            other += (pixel & 0xffffff) != 0xffffff && (pixel & 0xffffff) != 0;
        }
        check(white == 315 * 235 && other == 0, "integer image centered with crisp black borders");
        // An externally requested smooth-filter hint must not blur guest pixels.
        frame.assign(64 * 48, 0xff000000);
        for (unsigned y = 0; y < 48; ++y)
            for (unsigned x = 0; x < 64; ++x)
                if ((x + y) % 2) frame[y * 64 + x] = 0xffffffff;
        frontend->present(frame, 64, 48);
        check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                   output.data(), 320 * 4) == 0, "checkerboard output readable");
        other = 0;
        for (unsigned y = 0; y < 240; ++y)
            for (unsigned x = 0; x < 320; ++x)
                other += output[y * 320 + x] != frame[(y / 5) * 64 + x / 5];
        check(other == 0, "nearest sampling exactly repeats every guest pixel");
        frame.assign(640 * 480, 0xffffffff);
        frontend->present(frame, 640, 480);
        int manualWidth, manualHeight;
        SDL_GetWindowSize(window, &manualWidth, &manualHeight);
        check(manualWidth == 320 && manualHeight == 240, "manual window size survives mode changes");
        check(!SDL_RenderGetIntegerScale(renderer), "undersized windows fit instead of cropping");
        frontend->present(frame, 640, 480, DisplayShape::ClassicVga);
        check(!SDL_RenderGetIntegerScale(renderer), "classic VGA retains monitor aspect correction");
        check(frontend->capturePointer(false), "pointer release works without guest state");
        SDL_Event e{};
        e.type = SDL_KEYDOWN;
        e.key.keysym.scancode = SDL_SCANCODE_F12;
        e.key.keysym.sym = SDLK_F12;
        SDL_PushEvent(&e);
        e.type = SDL_KEYUP;
        SDL_PushEvent(&e);
        e = {};
        e.type = SDL_MOUSEBUTTONDOWN;
        e.button.button = SDL_BUTTON_RIGHT;
        SDL_PushEvent(&e);
        e = {};
        e.type = SDL_MOUSEMOTION;
        e.motion.xrel = 3;
        e.motion.yrel = -4;
        SDL_PushEvent(&e);
        e = {};
        e.type = SDL_MOUSEWHEEL;
        e.wheel.y = 1;
        e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
        SDL_PushEvent(&e);
        e = {};
        e.type = SDL_WINDOWEVENT;
        e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
        SDL_PushEvent(&e);
        e = {};
        e.type = SDL_KEYDOWN;
        e.key.keysym.sym = SDLK_g;
        e.key.keysym.mod = KMOD_GUI;
        SDL_PushEvent(&e);
        e.key.keysym.sym = SDLK_q;
        SDL_PushEvent(&e);
        auto events = frontend->poll();
        using K = FrontendEvent::Kind;
        check(events.size() == 8, "adapter normalizes exactly one event per host event");
        check(events[0].kind == K::Key && events[0].key == hid::KEY_F12 && !events[0].released &&
                  events[1].kind == K::Key && events[1].released,
              "physical F12 HID make/break");
        check(events[2].kind == K::Button && events[2].button == 2 && !events[2].released,
              "right mouse button identity");
        check(events[3].kind == K::Motion && events[3].x == 3 && events[3].y == -4,
              "relative motion keeps host axis convention");
        check(events[4].kind == K::Wheel && events[4].y == -1, "flipped wheel is normalized once");
        check(events[5].kind == K::FocusLost, "focus loss remains available to release guest keys");
        check(events[6].kind == K::ToggleCapture && events[7].kind == K::Quit,
              "host shortcuts stay distinct from guest input");
        frontend.reset();
        check(!SDL_WasInit(SDL_INIT_VIDEO), "frontend owns SDL resource lifetime");
        std::cout << "Passed " << checks << " frontend checks\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
