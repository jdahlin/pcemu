#include "NativeBridge.h"
#include "pc_session.hpp"
#include <mutex>
#include <atomic>
#include <algorithm>

namespace {
std::mutex mutex;
std::vector<pc::FrontendEvent> events;
class NativeControl final : public pc::SessionControl {
public:
    std::vector<std::string> commands;
    void reportError(std::string_view message) override;
    std::vector<std::string> takeCommands() override {
        std::lock_guard lock(mutex);
        std::vector<std::string> result;
        result.swap(commands);
        return result;
    }
} control;
std::vector<uint32_t> frame;
unsigned width = 0, height = 0;
bool classic = false, dirty = false;
std::string error;
void NativeControl::reportError(std::string_view message) {
    std::lock_guard lock(mutex); error = message;
}
pc::HostCursor cursor;
bool cursorDirty = false;
std::atomic<bool> captured{false}, absolute{false};
class NativeFrontend final : public pc::Frontend {
public:
    std::vector<pc::FrontendEvent> poll() override {
        std::lock_guard lock(mutex);
        std::vector<pc::FrontendEvent> result;
        result.swap(events);
        return result;
    }
    void present(std::span<const uint32_t> pixels, unsigned w, unsigned h,
                 pc::DisplayShape shape) override {
        std::lock_guard lock(mutex);
        frame.assign(pixels.begin(), pixels.end());
        width = w; height = h;
        classic = shape == pc::DisplayShape::ClassicVga;
        dirty = true;
    }
    void setHostCursor(const pc::HostCursor &next) override {
        std::lock_guard lock(mutex);
        if (cursor.active == next.active && cursor.visible == next.visible &&
            cursor.revision == next.revision && cursor.signature() == next.signature()) return;
        cursor = next; cursorDirty = true;
    }
    void reportError(std::string_view message) override {
        std::lock_guard lock(mutex); error = message;
    }
    void setAbsolutePointer(bool enabled) override { absolute = enabled; }
    bool capturePointer(bool enabled) override { captured = enabled; return enabled; }
};
}
extern "C" void pcemu_native_prepare() {
    {
        std::lock_guard lock(mutex);
        events.clear(); control.commands.clear(); frame.clear(); dirty = false; error.clear();
        captured = false; absolute = false; cursor = {}; cursorDirty = true;
    }
}
extern "C" int pcemu_native_run(const char *machine, const char *disk, const char *snapshot,
                                 const char *rom, unsigned memory, const char *bootText, int initializeVga) {
    pc::PcSessionOptions options;
    options.machine = machine; options.disk = disk; options.snapshot = snapshot;
    options.rom = rom; options.memoryMiB = memory;
    options.bootText = bootText; options.initializeVga = initializeVga;
    return pc::runPcSession(options,
        [](unsigned, unsigned, pc::DisplayShape) -> std::unique_ptr<pc::Frontend> {
            return std::make_unique<NativeFrontend>();
        }, &control);
}
extern "C" void pcemu_native_command(const char *command) {
    std::lock_guard lock(mutex);
    control.commands.emplace_back(command);
}
extern "C" int pcemu_native_error(char *message, int capacity) {
    std::lock_guard lock(mutex);
    if (error.empty() || capacity < 1) return 0;
    auto count = std::min(error.size(), size_t(capacity - 1));
    std::copy_n(error.data(), count, message); message[count] = 0;
    error.clear(); return 1;
}
extern "C" void pcemu_native_event(int kind, uint16_t key, int released, int button, int x, int y) {
    // Stable ABI event IDs, independent of enum layout.
    using K = pc::FrontendEvent::Kind;
    constexpr K kinds[] = {K::FocusLost, K::Key, K::ToggleCapture, K::Button,
                           K::Motion, K::AbsoluteMotion, K::PointerLeave, K::Wheel};
    if (kind < 0 || kind >= int(std::size(kinds))) return;
    std::lock_guard lock(mutex);
    events.push_back({kinds[kind], key, bool(released), uint8_t(button), x, y});
}
extern "C" int pcemu_native_frame(uint32_t *pixels, int capacity, int *w, int *h, int *c) {
    std::lock_guard lock(mutex);
    if (!dirty || capacity < 0 || size_t(capacity) < frame.size()) return 0;
    std::copy(frame.begin(), frame.end(), pixels);
    *w = int(width); *h = int(height); *c = classic;
    dirty = false;
    return int(frame.size());
}
extern "C" int pcemu_native_captured() { return captured; }
extern "C" int pcemu_native_absolute() { return absolute; }

extern "C" int pcemu_native_cursor(uint32_t *pixels, int *w, int *h, int *hx, int *hy, int *visible) {
    std::lock_guard lock(mutex);
    if (!cursorDirty) return 0;
    cursorDirty = false;
    *w = *h = 1; *hx = *hy = 0; *visible = 0; pixels[0] = 0;
    if (!cursor.active || !cursor.visible || cursor.size < 12 ||
        pc::HostCursor::length(cursor.shape) != cursor.size) return 1;
    *w = pc::HostCursor::word(cursor.shape, 4); *h = pc::HostCursor::word(cursor.shape, 6);
    *hx = pc::HostCursor::word(cursor.shape, 0); *hy = pc::HostCursor::word(cursor.shape, 2);
    *visible = 1;
    const unsigned stride = (unsigned(*w) + 7) / 8;
    for (int y = 0; y < *h; ++y) for (int x = 0; x < *w; ++x) {
        uint32_t pixel;
        if (cursor.shape[11] != 1) pixel = cursor.colorPixel(x, y);
        else {
            const auto bit = 0x80 >> (x % 8);
            bool mask = cursor.shape[12 + y * stride + x / 8] & bit;
            bool color = cursor.shape[12 + stride * *h + y * stride + x / 8] & bit;
            // Cocoa ARGB cannot invert the desktop; approximate inversion as white.
            pixel = mask && !color ? 0 : color ? 0xffffffff : 0xff000000;
        }
        pixels[y * *w + x] = pixel;
    }
    return 1;
}
