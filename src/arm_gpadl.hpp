#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>
namespace pc {
// Collects VMBus GPA-range descriptors. Host access is allowed only after the
// entire descriptor is validated, and only for its owning channel. All VPs
// must be stopped during collection and guest-memory transfers.
class ArmGpadl {
  public:
    enum class Result { Ok, Invalid, Missing, WrongChannel, InUse, Limit };
    static constexpr size_t maxDescriptors = 64;
    ArmGpadl(std::span<uint8_t> ram, uint64_t base);
    Result begin(uint32_t channel, uint32_t id, uint16_t bytes, uint16_t ranges,
                 std::span<const uint8_t> fragment);
    Result append(uint32_t id, std::span<const uint8_t> fragment);
    Result remove(uint32_t channel, uint32_t id);
    void clearChannel(uint32_t channel);
    std::optional<uint32_t> owner(uint32_t id) const;
    std::optional<uint64_t> byteSize(uint32_t channel, uint32_t id) const;
    // Transfer buffers must not overlap guest RAM.
    bool read(uint32_t channel, uint32_t id, uint64_t offset, std::span<uint8_t> out) const;
    bool write(uint32_t channel, uint32_t id, uint64_t offset, std::span<const uint8_t> in);
    size_t count() const { return descriptors.size(); }
  private:
    struct Range { uint32_t offset, bytes; std::vector<uint64_t> pages; };
    struct Descriptor {
        uint32_t channel;
        uint16_t expectedBytes, expectedRanges;
        std::vector<uint8_t> encoded;
        std::vector<Range> ranges;
        uint64_t bytes = 0;
        bool ready = false;
    };
    std::span<uint8_t> ram;
    uint64_t base;
    std::map<uint32_t, Descriptor> descriptors;
    bool finish(Descriptor &descriptor) const;
    bool aliasesRam(std::span<const uint8_t> bytes) const;
    const Descriptor *find(uint32_t channel, uint32_t id) const;
    // A piece never crosses a range or physical-page boundary.
    std::span<uint8_t> piece(const Descriptor &descriptor, uint64_t offset, size_t maximum) const;
};
}
