#pragma once
#include "pointer_shape.hpp"
#include "virtio_device.hpp"
#include <array>
#include <map>
namespace pc {
// CPU-backed VirtIO GPU 2D: one scanout, EDID, fenced control commands and cursor.
class VirtioGpu : public VirtioDevice {
  public:
    VirtioGpu(std::span<uint8_t> ram, uint64_t base, Interrupt irq);
    bool active() const {
        return scanoutResource != 0;
    }
    bool pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height,
                bool compositeCursor = true) const;
    bool changedPixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height,
                       bool compositeCursor = true) const;
    PointerShape pointerShape() const;
    std::string mode() const;
    void save(const std::string &path) const;
    uint64_t commands = 0, commandErrors = 0, transfers = 0, flushes = 0, cursorUpdates = 0;
    uint64_t scanoutCommands = 0;
    uint32_t lastErrorCommand = 0, lastErrorResponse = 0;
    size_t resourceBytes() const {
        return allocated;
    }

  private:
    struct Rect {
        uint32_t x, y, width, height;
    };
    struct Backing {
        uint64_t address;
        uint32_t length;
    };
    struct Resource {
        uint32_t width, height, format;
        std::vector<uint8_t> data;
        std::vector<Backing> backing;
        uint64_t backingBytes = 0;
    };
    static constexpr size_t memoryLimit = 256 * 1024 * 1024;
    std::map<uint32_t, Resource> resources;
    size_t allocated = 0;
    std::vector<uint32_t> lastFrame;
    unsigned lastWidth = 0, lastHeight = 0;
    void rememberFrame();
    uint32_t scanoutResource = 0, cursorResource = 0, cursorX = 0, cursorY = 0, hotX = 0, hotY = 0;
    Rect scanout{};
    uint32_t events = 0;
    uint64_t generation = 1;
    mutable uint64_t displayedGeneration = 0;
    mutable bool displayedComposite = true;
    std::optional<uint32_t> handle(unsigned queue, const Chain &chain) override;
    std::vector<uint8_t> control(std::span<const uint8_t> request, size_t responseCapacity);
    void cursor(std::span<const uint8_t> request);
    uint64_t configRead(unsigned offset, unsigned width) const override;
    void configWrite(unsigned offset, uint64_t value, unsigned width) override;
    void deviceReset() override;
    void readBacking(const Resource &resource, uint64_t offset, std::span<uint8_t> output) const;
    static uint64_t get(std::span<const uint8_t> bytes, size_t offset, unsigned width = 4);
    static void put(std::vector<uint8_t> &bytes, size_t offset, uint64_t value, unsigned width = 4);
    static Rect rect(std::span<const uint8_t> bytes);
    static bool contains(const Resource &resource, Rect rectangle);
    static uint32_t pixel(const Resource &resource, size_t index);
    static std::array<uint8_t, 128> edid();
};
} // namespace pc
