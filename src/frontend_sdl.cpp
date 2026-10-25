#include "frontend.hpp"
#include <SDL.h>
#include <algorithm>
#include <stdexcept>
namespace pc {
namespace {
class SDLFrontend final : public Frontend {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    unsigned width = 0, height = 0;
    void destroy() {
        if (texture)
            SDL_DestroyTexture(texture);
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
    }

  public:
    SDLFrontend(unsigned w, unsigned h, DisplayShape shape) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER))
            throw std::runtime_error(SDL_GetError());
        try {
            int ww = std::max(w, 960u);
            int wh = shape == DisplayShape::ClassicVga ? ww * 3 / 4 : uint64_t(ww) * h / w;
            SDL_Rect bounds{};
            if (!SDL_GetDisplayUsableBounds(0, &bounds)) {
                double scale = std::min({1.0, double(std::max(bounds.w - 40, 320)) / ww,
                                         double(std::max(bounds.h - 80, 240)) / wh});
                ww = int(ww * scale);
                wh = int(wh * scale);
            }
            if (shape == DisplayShape::ClassicVga) {
                ww = std::min(ww / 4, wh / 3) * 4;
                wh = ww / 4 * 3;
            }
            window = SDL_CreateWindow("PCEmu — click to capture mouse (Cmd+G)",
                                      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                                      SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!window)
                throw std::runtime_error(SDL_GetError());
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
            if (!renderer)
                renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            if (!renderer)
                throw std::runtime_error(SDL_GetError());
            SDL_StopTextInput();
        } catch (...) {
            destroy();
            throw;
        }
    }
    ~SDLFrontend() override {
        destroy();
    }
    bool capturePointer(bool enabled) override {
        bool success = SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0;
        bool captured = enabled && success;
        SDL_SetWindowTitle(window, captured ? "PCEmu — mouse captured (Cmd+G to release)"
                                            : "PCEmu — click to capture mouse (Cmd+G)");
        return !enabled || success;
    }
    std::vector<FrontendEvent> poll() override {
        std::vector<FrontendEvent> events;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            using K = FrontendEvent::Kind;
            if (e.type == SDL_QUIT)
                events.push_back({K::Quit});
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                events.push_back({K::FocusLost});
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                bool up = e.type == SDL_KEYUP;
                if (!up && (e.key.keysym.mod & KMOD_GUI) && e.key.keysym.sym == SDLK_q)
                    events.push_back({K::Quit});
                else if (!up && (e.key.keysym.mod & KMOD_GUI) && e.key.keysym.sym == SDLK_g)
                    events.push_back({K::ToggleCapture});
                else
                    events.push_back({K::Key, uint16_t(e.key.keysym.scancode), up});
            } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                uint8_t bit = e.button.button == SDL_BUTTON_LEFT     ? 1
                              : e.button.button == SDL_BUTTON_RIGHT  ? 2
                              : e.button.button == SDL_BUTTON_MIDDLE ? 4
                                                                     : 0;
                events.push_back({K::Button, 0, e.type == SDL_MOUSEBUTTONUP, bit});
            } else if (e.type == SDL_MOUSEMOTION)
                events.push_back({K::Motion, 0, false, 0, e.motion.xrel, e.motion.yrel});
            else if (e.type == SDL_MOUSEWHEEL)
                events.push_back(
                    {K::Wheel, 0, false, 0, 0,
                     e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y});
        }
        return events;
    }
    void present(std::span<const uint32_t> pixels, unsigned w, unsigned h, DisplayShape shape) override {
        if (!texture || w != width || h != height) {
            if (texture)
                SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, w, h);
            if (!texture)
                throw std::runtime_error(SDL_GetError());
            width = w;
            height = h;

        }
        // Logical output dimensions describe the monitor, independently of the texture.
        // Reapply even when only the mode policy changes at the same resolution.
        SDL_RenderSetLogicalSize(renderer, shape == DisplayShape::ClassicVga ? 4 : w,
                                 shape == DisplayShape::ClassicVga ? 3 : h);
        SDL_UpdateTexture(texture, nullptr, pixels.data(), w * 4);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }
};
} // namespace
std::unique_ptr<Frontend> createFrontend(unsigned w, unsigned h, DisplayShape shape) {
    return std::make_unique<SDLFrontend>(w, h, shape);
}
} // namespace pc
