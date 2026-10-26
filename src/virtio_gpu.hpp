#pragma once
#include "pointer_shape.hpp"
#include "virtio_device.hpp"
#include <array>
#include <map>
namespace pc {
// CPU-backed VirtIO GPU 2D: one scanout, EDID, fenced control commands and cursor.
class VirtioGpu : public VirtioDevice {
  public:
    // Bind the two 2D GPU queues and offer monitor EDID support.
    VirtioGpu(std::span<uint8_t> ram, uint64_t base, Interrupt irq);
    // Report whether a scanout resource is currently selected.
    // IMPLEMENTATION NOTE: Host observation of GPU resource/scanout state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3650007
    bool active() const {
        return scanoutResource != 0;
    }
    // Capture the active scanout as opaque ARGB, optionally alpha-compositing the cursor.
    bool read_pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height,
                     bool compositeCursor = true) const;
    // Capture only when display generation or cursor-compositing mode changed.
    bool changed_pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height,
                        bool compositeCursor = true) const;
    // Export a separate cursor image/hotspot for the host frontend without compositing it.
    PointerShape pointer_shape() const;
    // Describe the active scanout dimensions or report inactive.
    std::string mode_description() const;
    // Export current or last captured display as binary PPM; fail if none exists.
    void save_ppm(const std::string &path) const;
    uint64_t commands = 0, commandErrors = 0, transfers = 0, flushes = 0, cursorUpdates = 0;
    uint64_t scanoutCommands = 0;
    uint32_t lastErrorCommand = 0, lastErrorResponse = 0;
    // Inspect total host-private resource pixel bytes allocated.
    // IMPLEMENTATION NOTE: Host observation of GPU resource/scanout state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3650007
    size_t resource_bytes() const {
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
    // IMPLEMENTATION NOTE: Bound total host pixel storage to 256 MiB.
    static constexpr size_t MaximumResourceBytes = 256 * 1024 * 1024;
    std::map<uint32_t, Resource> resources;
    size_t allocated = 0;
    std::vector<uint32_t> lastFrame;
    unsigned lastWidth = 0, lastHeight = 0;
    // Cache the currently active composited display before its resource is detached/reset.
    void remember_frame();
    uint32_t scanoutResource = 0, cursorResource = 0, cursorX = 0, cursorY = 0, hotX = 0, hotY = 0;
    Rect scanout{};
    uint32_t events = 0;
    uint64_t generation = 1;
    mutable uint64_t displayedGeneration = 0;
    mutable bool displayedComposite = true;
    // Dispatch a readable command chain to controlq/cursorq and write any control response.
    std::optional<uint32_t> handle(unsigned queue, const Chain &chain) override;
    // Execute supported 2D control commands and return a fenced success/error response.
    std::vector<uint8_t> execute_control_command(std::span<const uint8_t> request,
                                                 size_t responseCapacity);
    // Validate cursor update/move state and advance host display generation; count command errors.
    void execute_cursor_command(std::span<const uint8_t> request);
    // Read events and one-scanout/no-capset configuration bytes.
    uint64_t read_device_configuration(unsigned offset, unsigned width) const override;
    // Clear only event bits written through events_clear; ignore other fields.
    void write_device_configuration(unsigned offset, uint64_t value, unsigned width) override;
    // Preserve the last frame, clear resources/scanout/cursor/events and advance host generation.
    void reset_device() override;
    // Copy a validated slice from the resource’s scatter/gather guest backing into host pixels.
    void read_resource_backing(const Resource &resource, uint64_t offset,
                               std::span<uint8_t> output) const;
    // Decode a bounded little-endian command field or return INVALID_PARAMETER via CommandError.
    static uint64_t read_command_field(std::span<const uint8_t> bytes, size_t offset,
                                       unsigned width = 4);
    // Encode a little-endian response field into its sized host buffer.
    static void write_response_field(std::vector<uint8_t> &bytes, size_t offset, uint64_t value,
                                     unsigned width = 4);
    // Decode x/y/width/height following the common command header.
    static Rect read_rectangle(std::span<const uint8_t> bytes);
    // Check a nonempty rectangle lies entirely within the resource without overflow.
    static bool contains(const Resource &resource, Rect rectangle);
    // Decode one supported eight-bit normalized format into host ARGB without forcing opacity.
    static uint32_t decode_pixel(const Resource &resource, size_t index);
    // Build the retained 128-byte virtual-monitor EDID and its checksum.
    static std::array<uint8_t, 128> edid();
};
} // namespace pc
