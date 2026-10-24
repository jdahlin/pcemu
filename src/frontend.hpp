#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
namespace pc {
// UI events use USB HID keyboard usages, never toolkit keycodes or guest scan codes.
struct FrontendEvent {
    enum class Kind { Quit, FocusLost, Key, ToggleCapture, Button, Motion, Wheel };
    Kind kind;
    uint16_t key = 0;
    bool released = false;
    uint8_t button = 0; // Left=1, right=2, middle=4.
    int x = 0, y = 0;   // Relative motion in guest pixels; positive Y is down.
};
class Frontend {
  public:
    virtual ~Frontend() = default;
    virtual std::vector<FrontendEvent> poll() = 0;
    // Pixels are borrowed only for this call, in 0xAARRGGBB format.
    virtual void present(std::span<const uint32_t> pixels, unsigned width, unsigned height) = 0;
    virtual bool capturePointer(bool enabled) = 0;
};
// Each frontend executable supplies one factory. The machine and execution
// engines do not depend on a GUI toolkit or own its event loop.
std::unique_ptr<Frontend> createFrontend(unsigned width, unsigned height);
} // namespace pc
