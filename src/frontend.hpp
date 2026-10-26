#pragma once
#include "host_cursor.hpp"
#include "pointer_shape.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
namespace pc {
// UI events use USB HID keyboard usages, never toolkit keycodes or guest scan codes.
struct FrontendEvent {
    enum class Kind { Quit, FocusLost, Key, ToggleCapture, Button, Motion, AbsoluteMotion, PointerLeave, Wheel, Redraw };
    Kind kind;
    uint16_t key = 0;
    bool released = false;
    uint8_t button = 0; // Left=1, right=2, middle=4.
    int x = 0, y = 0;   // Motion: relative counts, Y down. AbsoluteMotion: 0..65535.
};
enum class DisplayShape { SquarePixels, ClassicVga };
class Frontend {
  public:
    virtual ~Frontend() = default;
    virtual std::vector<FrontendEvent> poll() = 0;
    virtual void reportError(std::string_view) {}
    // Pixels are borrowed only for this call, in 0xAARRGGBB format.
    virtual void present(std::span<const uint32_t> pixels, unsigned width, unsigned height,
                         DisplayShape shape = DisplayShape::SquarePixels) = 0;
    // Absolute motion uses 0..65535 coordinates over the visible guest image.
    virtual void setAbsolutePointer(bool) {}
    virtual void setHostCursor(const HostCursor &) {}
    virtual void setPointerShape(const PointerShape &) {}
    virtual bool capturePointer(bool enabled) = 0;
};
// Each frontend executable supplies one factory. The machine and execution
// engines do not depend on a GUI toolkit or own its event loop.
std::unique_ptr<Frontend> createFrontend(unsigned width, unsigned height,
                                         DisplayShape shape = DisplayShape::SquarePixels);
} // namespace pc
