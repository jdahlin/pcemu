#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>
namespace pc {
// VMBus Guest Physical Address Descriptor List (GPADL) wire/lifecycle helper.
// Primary wire reference: Linux gpa_range and VMBus GPADL messages, not a
// claim that a complete standalone Microsoft VMBus specification is implemented.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L594
// Collects VMBus GPA-range descriptors. Host access is allowed only after the
// entire descriptor is validated, and only for its owning channel. All VPs
// must be stopped during collection and guest-memory transfers.
class ArmGpadl {
  public:
    enum class Result { Ok, Invalid, Missing, WrongChannel, InUse, Limit };
    // IMPLEMENTATION NOTE: Bound simultaneous host descriptors; not a protocol maximum.
    static constexpr size_t maxDescriptors = 64;
    // Hyper-V GPA page numbers use 4 KiB units even on hosts with larger pages.
    // A GPA range is byte-count u32, first-page byte-offset u32, then page-frame
    // numbers u64. PFN count is ceil((offset+count)/4096), not a separate field.
    // https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
    static constexpr uint64_t GuestPageBytes = 4096, GuestPageOffsetMask = GuestPageBytes - 1;
    static constexpr unsigned GuestPageShift = 12, RangeHeaderBytes = 8, PageFrameNumberBytes = 8;
    // Encoded ranges/fragments are multiples of eight; one nonempty range needs
    // its eight-byte header and at least one eight-byte page-frame number.
    static constexpr unsigned FragmentAlignmentMask = PageFrameNumberBytes - 1,
                              MinimumRangeBytes = RangeHeaderBytes + PageFrameNumberBytes;
    // Bind RAM with a 4 KiB aligned guest physical base; throw on bad alignment.
    ArmGpadl(std::span<uint8_t> ram, uint64_t base);
    // Reserve a new channel-owned handle and collect its first encoded range fragment.
    Result begin_descriptor(uint32_t channel, uint32_t id, uint16_t bytes, uint16_t ranges,
                            std::span<const uint8_t> fragment);
    // Append a nonempty aligned fragment; validate the final candidate before replacing state.
    Result append_descriptor(uint32_t id, std::span<const uint8_t> fragment);
    // Release a handle only for its owner, invalidating existing host ring leases.
    Result teardown_descriptor(uint32_t channel, uint32_t id);
    // Remove all mappings owned by a channel during host channel teardown.
    void clear_channel(uint32_t channel);
    // Return the owning channel of a known handle, even before fragment collection completes.
    std::optional<uint32_t> descriptor_owner(uint32_t id) const;
    // Report encoded bytes still required by a known handle, including incomplete mappings.
    std::optional<size_t> remaining_descriptor_bytes(uint32_t id) const;
    // Return the concatenated byte length only for a ready mapping owned by this channel.
    std::optional<uint64_t> mapped_byte_count(uint32_t channel, uint32_t id) const;
    // Transfer buffers must not overlap guest RAM.
    // Copy a bounded channel-owned mapping to non-overlapping host storage; false leaves output
    // untouched.
    bool read_mapped_bytes(uint32_t channel, uint32_t id, uint64_t offset,
                           std::span<uint8_t> out) const;
    // Copy non-overlapping host storage into a bounded channel-owned mapping; invalid requests
    // write nothing.
    bool write_mapped_bytes(uint32_t channel, uint32_t id, uint64_t offset,
                            std::span<const uint8_t> in);
    // Return the host lease generation of a ready mapping; missing/reused handles invalidate rings.
    std::optional<uint64_t> mapping_generation(uint32_t channel, uint32_t id) const;
    // Ring mappings require full pages and no physical aliases, including headers.
    // Validate a complete page-aligned ring window and reject aliases anywhere in its descriptor.
    bool valid_ring_mapping(uint32_t channel, uint32_t id, uint64_t offset, uint64_t bytes) const;
    // Count host descriptor records, including mappings still being collected.
    // IMPLEMENTATION NOTE: Diagnostic state for GPADL lifecycle.
    // https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L594
    size_t descriptor_count() const {
        return descriptors.size();
    }

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    struct Range {
        uint32_t offset, bytes;
        std::vector<uint64_t> pages;
    };
    struct Descriptor {
        uint32_t channel;
        uint16_t expectedBytes, expectedRanges;
        std::vector<uint8_t> encoded;
        std::vector<Range> ranges;
        uint64_t bytes = 0;
        bool ready = false;
        uint64_t generation = 0;
    };
    std::span<uint8_t> ram;
    uint64_t base, nextGeneration = 0;
    std::map<uint32_t, Descriptor> descriptors;
    // Decode all GPA ranges/PFNs into validated RAM pages and mark the complete descriptor ready.
    bool validate_descriptor(Descriptor &descriptor) const;
    // Detect any overlap between a transfer buffer and guest RAM using host pointer ranges.
    bool overlaps_guest_ram(std::span<const uint8_t> bytes) const;
    // Look up a ready descriptor only when its channel owner matches; otherwise return null.
    const Descriptor *ready_descriptor(uint32_t channel, uint32_t id) const;
    // A piece never crosses a range or physical-page boundary.
    // Translate a logical offset into one host span bounded by range end, page end and requested
    // size.
    std::span<uint8_t> mapped_piece(const Descriptor &descriptor, uint64_t offset,
                                    size_t maximum) const;
};
} // namespace pc
