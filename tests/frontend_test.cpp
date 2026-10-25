#include "frontend.hpp"
#include "host_key.hpp"
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
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
        auto frontend = createFrontend(640, 400, DisplayShape::ClassicVga);
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
        std::vector<uint32_t> frame(640 * 400, 0xff123456);
        frontend->present(frame, 640, 400);
        frame.resize(1024 * 768, 0xffabcdef);
        frontend->present(frame, 1024, 768);
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
