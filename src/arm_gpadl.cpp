// A VMBus Guest Physical Address Descriptor List (GPADL) lets guest software
// share scattered guest RAM pages as logically contiguous ranges. Each range
// describes a byte count, an offset within its first 4 KiB page and the page-frame
// numbers (PFNs) covering those bytes. Multiple ranges are concatenated in this
// helper's logical address space. Channel control messages supply a header and
// optional continuation fragments; ArmVmbus owns message validation/replies.
//
// This helper collects fragments without exposing partial mappings. Completion
// decodes every range, checks every physical page against RAM and commits the
// ready descriptor only if the entire encoding is valid. Transfers require the
// correct owning channel, a ready descriptor and bounds covering the whole copy.
// Ring construction additionally requires full, distinct pages. Teardown removes
// the handle; monotonically assigned host generations invalidate old ring objects
// even when the guest immediately reuses the same numeric handle.
//
// IMPLEMENTATION NOTE: All virtual processors must be stopped during collection
// and transfers. Only RAM is supported; transfer buffers must not alias guest RAM.
// The descriptor cap, nonzero channel/handle rule, strict eight-byte fragment
// alignment, global per-descriptor ring-alias rejection and generation exhaustion
// handling are host validation/lifetime policies. A ring needs one header page
// and at least one data page. These checks do not allocate guest RAM, pin native
// host pages or implement VMBus control-message sequencing by themselves.
// No separate complete VMBus specification is used here: the primary wire reference
// is Linux's gpa_range/vmbus_channel_gpadl_header/body/teardown definitions.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L594

#include "arm_gpadl.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace pc {
namespace {
// Decode a little-endian GPA range/PFN field after the caller has checked its bounds.
// Linux gpa_range wire layout.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
uint64_t read_little_endian(std::span<const uint8_t> bytes, size_t at, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= uint64_t(bytes[at + i]) << (8 * i);
    return value;
}
} // namespace
// Bind RAM and reject an unaligned guest base before collecting any descriptors.
// IMPLEMENTATION NOTE: RAM-only host mapping of VMBus GPA ranges.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
ArmGpadl::ArmGpadl(std::span<uint8_t> memory, uint64_t start) : ram(memory), base(start) {
    if (base & GuestPageOffsetMask)
        throw std::runtime_error("GPADL RAM base must be 4 KiB aligned");
}
// Reserve a new channel-owned handle and collect its first encoded range fragment.
// Validate immediately if complete; reject duplicates, malformed lengths and host limits.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L594
ArmGpadl::Result ArmGpadl::begin_descriptor(uint32_t channel, uint32_t id, uint16_t bytes,
                                            uint16_t ranges, std::span<const uint8_t> fragment) {
    if (!channel || !id || !ranges || !bytes || (bytes & FragmentAlignmentMask) ||
        bytes < uint32_t(ranges) * MinimumRangeBytes || fragment.size() > bytes ||
        (fragment.size() & FragmentAlignmentMask))
        return Result::Invalid;
    if (descriptors.contains(id))
        return Result::InUse;
    if (descriptors.size() >= maxDescriptors)
        return Result::Limit;
    Descriptor candidate{channel, bytes, ranges, {fragment.begin(), fragment.end()}, {}, 0, false};
    if (candidate.encoded.size() == bytes && !validate_descriptor(candidate))
        return Result::Invalid;
    if (nextGeneration == UINT64_MAX)
        return Result::Limit;
    candidate.generation = ++nextGeneration;
    descriptors.emplace(id, std::move(candidate));
    return Result::Ok;
}
// Append a nonempty aligned fragment; validate the final candidate before replacing state.
// Invalid final fragments leave the prior collected bytes intact so callers can reject/teardown.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L604
ArmGpadl::Result ArmGpadl::append_descriptor(uint32_t id, std::span<const uint8_t> fragment) {
    auto it = descriptors.find(id);
    if (it == descriptors.end())
        return Result::Missing;
    const auto &existing = it->second;
    if (existing.ready)
        return Result::InUse;
    if (fragment.empty() || (fragment.size() & FragmentAlignmentMask) ||
        fragment.size() > existing.expectedBytes - existing.encoded.size())
        return Result::Invalid;
    if (fragment.size() < existing.expectedBytes - existing.encoded.size()) {
        it->second.encoded.insert(it->second.encoded.end(), fragment.begin(), fragment.end());
        return Result::Ok;
    }
    // Reject malformed completion without consuming the previous fragments.
    auto candidate = existing;
    candidate.encoded.insert(candidate.encoded.end(), fragment.begin(), fragment.end());
    if (candidate.encoded.size() == candidate.expectedBytes && !validate_descriptor(candidate))
        return Result::Invalid;
    it->second = std::move(candidate);
    return Result::Ok;
}
// Decode all GPA ranges/PFNs into validated RAM pages and mark the complete descriptor ready.
// Require exact declared range count and byte consumption before publishing any access.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
bool ArmGpadl::validate_descriptor(Descriptor &descriptor) const {
    std::vector<Range> ranges;
    uint64_t total = 0;
    size_t at = 0;
    const auto &bytes = descriptor.encoded;
    for (unsigned i = 0; i < descriptor.expectedRanges; ++i) {
        if (at > bytes.size() || bytes.size() - at < RangeHeaderBytes)
            return false;
        // Range header: byte count followed by first-page byte offset, both little endian.
        const auto count = uint32_t(read_little_endian(bytes, at, 4)),
                   offset = uint32_t(read_little_endian(bytes, at + 4, 4));
        if (!count || offset >= GuestPageBytes)
            return false;
        const uint64_t pages = (uint64_t(offset) + count + GuestPageOffsetMask) / GuestPageBytes;
        at += RangeHeaderBytes;
        if (pages > (bytes.size() - at) / PageFrameNumberBytes)
            return false;
        Range range{offset, count, {}};
        range.pages.reserve(size_t(pages));
        for (uint64_t page = 0; page < pages; ++page) {
            auto pfn = read_little_endian(bytes, at, PageFrameNumberBytes);
            at += PageFrameNumberBytes;
            if (pfn > (UINT64_MAX >> GuestPageShift))
                return false;
            auto gpa = pfn << GuestPageShift;
            if (gpa < base || gpa - base > ram.size() || GuestPageBytes > ram.size() - (gpa - base))
                return false;
            range.pages.push_back(gpa);
        }
        total += count;
        ranges.push_back(std::move(range));
    }
    if (at != bytes.size())
        return false;
    descriptor.ranges = std::move(ranges);
    descriptor.bytes = total;
    descriptor.ready = true;
    return true;
}
// Look up a ready descriptor only when its channel owner matches; otherwise return null.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L594
const ArmGpadl::Descriptor *ArmGpadl::ready_descriptor(uint32_t channel, uint32_t id) const {
    auto it = descriptors.find(id);
    return it != descriptors.end() && it->second.channel == channel && it->second.ready
               ? &it->second
               : nullptr;
}
// Report encoded bytes still required by a known handle, including incomplete mappings.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L594
std::optional<size_t> ArmGpadl::remaining_descriptor_bytes(uint32_t id) const {
    auto it = descriptors.find(id);
    if (it == descriptors.end())
        return {};
    return it->second.expectedBytes - it->second.encoded.size();
}
// Return the owning channel of a known handle, even before fragment collection completes.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L594
std::optional<uint32_t> ArmGpadl::descriptor_owner(uint32_t id) const {
    auto it = descriptors.find(id);
    if (it == descriptors.end())
        return {};
    return it->second.channel;
}
// Return the concatenated byte length only for a ready mapping owned by this channel.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
std::optional<uint64_t> ArmGpadl::mapped_byte_count(uint32_t channel, uint32_t id) const {
    auto descriptor = ready_descriptor(channel, id);
    if (!descriptor)
        return {};
    return descriptor->bytes;
}
// Return the host lease generation of a ready mapping; missing/reused handles invalidate rings.
// IMPLEMENTATION NOTE: Generation is host lifetime bookkeeping, not a guest wire field.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L618
std::optional<uint64_t> ArmGpadl::mapping_generation(uint32_t channel, uint32_t id) const {
    auto descriptor = ready_descriptor(channel, id);
    if (!descriptor)
        return {};
    return descriptor->generation;
}
// Validate a complete page-aligned ring window and reject aliases anywhere in its descriptor.
// IMPLEMENTATION NOTE: Require header plus at least one data page, and no repeated PFNs
// even outside this ring window; this protects shared ring state from aliasing.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L114
bool ArmGpadl::valid_ring_mapping(uint32_t channel, uint32_t id, uint64_t offset,
                                  uint64_t bytes) const {
    const auto descriptor = ready_descriptor(channel, id);
    if (!descriptor || (offset & GuestPageOffsetMask) || (bytes & GuestPageOffsetMask) ||
        bytes < 2 * GuestPageBytes || offset > descriptor->bytes ||
        bytes > descriptor->bytes - offset)
        return false;
    std::set<uint64_t> pages;
    for (const auto &range : descriptor->ranges) {
        if (range.offset || (range.bytes & GuestPageOffsetMask))
            return false;
        for (auto page : range.pages)
            if (!pages.insert(page).second)
                return false;
    }
    return true;
}
// Translate a logical offset into one host span bounded by range end, page end and requested size.
// Logical offsets concatenate declared ranges; each range starts at its own first-page byte offset.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
std::span<uint8_t> ArmGpadl::mapped_piece(const Descriptor &descriptor, uint64_t offset,
                                          size_t maximum) const {
    for (const auto &range : descriptor.ranges) {
        if (offset >= range.bytes) {
            offset -= range.bytes;
            continue;
        }
        const uint64_t physical = range.offset + offset;
        const auto inPage = size_t(physical & GuestPageOffsetMask);
        const auto length = size_t(std::min<uint64_t>(
            std::min<size_t>(maximum, GuestPageBytes - inPage), range.bytes - offset));
        return ram.subspan(size_t(range.pages[size_t(physical / GuestPageBytes)] - base) + inPage,
                           length);
    }
    return {};
}
// Detect any overlap between a transfer buffer and guest RAM using host pointer ranges.
// IMPLEMENTATION NOTE: Reject aliasing to keep multi-piece copies independent of their own writes.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
bool ArmGpadl::overlaps_guest_ram(std::span<const uint8_t> bytes) const {
    if (bytes.empty() || ram.empty())
        return false;
    const auto start = reinterpret_cast<uintptr_t>(bytes.data());
    const auto memory = reinterpret_cast<uintptr_t>(ram.data());
    return start >= memory ? start - memory < ram.size() : bytes.size() > memory - start;
}
// Copy a bounded channel-owned mapping to non-overlapping host storage; false leaves output
// untouched. Validate ownership/whole-request bounds before copying across range/page boundaries.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
bool ArmGpadl::read_mapped_bytes(uint32_t channel, uint32_t id, uint64_t offset,
                                 std::span<uint8_t> out) const {
    auto descriptor = ready_descriptor(channel, id);
    if (!descriptor || offset > descriptor->bytes || out.size() > descriptor->bytes - offset ||
        overlaps_guest_ram(out))
        return false;
    while (!out.empty()) {
        auto part = mapped_piece(*descriptor, offset, out.size());
        std::copy(part.begin(), part.end(), out.begin());
        offset += part.size();
        out = out.subspan(part.size());
    }
    return true;
}
// Copy non-overlapping host storage into a bounded channel-owned mapping; invalid requests write
// nothing. Validate ownership/whole-request bounds before copying across range/page boundaries.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L388
bool ArmGpadl::write_mapped_bytes(uint32_t channel, uint32_t id, uint64_t offset,
                                  std::span<const uint8_t> in) {
    auto descriptor = ready_descriptor(channel, id);
    if (!descriptor || offset > descriptor->bytes || in.size() > descriptor->bytes - offset ||
        overlaps_guest_ram(in))
        return false;
    while (!in.empty()) {
        auto part = mapped_piece(*descriptor, offset, in.size());
        std::copy_n(in.begin(), part.size(), part.begin());
        offset += part.size();
        in = in.subspan(part.size());
    }
    return true;
}
// Release a handle only for its owner, invalidating existing host ring leases.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L618
ArmGpadl::Result ArmGpadl::teardown_descriptor(uint32_t channel, uint32_t id) {
    auto it = descriptors.find(id);
    if (it == descriptors.end())
        return Result::Missing;
    if (it->second.channel != channel)
        return Result::WrongChannel;
    descriptors.erase(it);
    return Result::Ok;
}
// Remove all mappings owned by a channel during host channel teardown.
// IMPLEMENTATION NOTE: Incomplete and completed handles are both released.
// Linux VMBus GPADL/ring wire interface; host restrictions are labeled above.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L618
void ArmGpadl::clear_channel(uint32_t channel) {
    for (auto it = descriptors.begin(); it != descriptors.end();) {
        if (it->second.channel == channel)
            it = descriptors.erase(it);
        else
            ++it;
    }
}
} // namespace pc
