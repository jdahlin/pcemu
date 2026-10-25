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
    int autoWidth = 0, autoHeight = 0;
    unsigned desktopWidth = 0, desktopHeight = 0;
    void fitWindow(unsigned w, unsigned h) {
        int ww = w, wh = h;
        SDL_Rect bounds{};
        if (!SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window), &bounds)) {
            const double scale = std::min({1.0, double(std::max(bounds.w - 40, 320)) / w,
                                          double(std::max(bounds.h - 80, 240)) / h});
            ww = int(w * scale);
            wh = int(h * scale);
        }
        SDL_SetWindowSize(window, ww, wh);
    }
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
        // Relative device counts must not be scaled into the monitor's logical
        // 4x3 coordinates. Guest drivers apply their own pointer acceleration.
        SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SCALING, "0", SDL_HINT_OVERRIDE);
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER))
            throw std::runtime_error(SDL_GetError());
        try {
            int ww = shape == DisplayShape::ClassicVga ? std::max(w, 960u) : w;
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
            autoWidth = ww;
            autoHeight = wh;
            if (shape == DisplayShape::SquarePixels) {
                desktopWidth = w;
                desktopHeight = h;
            }
            #ifdef __APPLE__
            // Metal nextDrawable can block for a second when this window is
            // occluded. Rendering shares the VM thread: never stall guest CPUs
            // behind the compositor. The software surface has no drawable pool.
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
#else
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
#endif
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
            SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
            width = w;
            height = h;
        }
        if (shape == DisplayShape::SquarePixels && (w != desktopWidth || h != desktopHeight)) {
            int currentWidth, currentHeight;
            SDL_GetWindowSize(window, &currentWidth, &currentHeight);
            // Follow desktop modes naturally until the user resizes the window.
            if (currentWidth == autoWidth && currentHeight == autoHeight) {
                fitWindow(w, h);
                SDL_GetWindowSize(window, &autoWidth, &autoHeight);
            }
            desktopWidth = w;
            desktopHeight = h;
        }
        int outputWidth = 0, outputHeight = 0;
        SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight);
        // Use physical output pixels, including Retina backing pixels. Fall back
        // to a fitted downscale only when native pixels cannot fit at all.
        SDL_RenderSetIntegerScale(renderer, shape == DisplayShape::SquarePixels &&
                                  outputWidth >= int(w) && outputHeight >= int(h) ? SDL_TRUE : SDL_FALSE);
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
