// A VMBus ring is a shared-memory byte queue: a 4 KiB header contains producer
// WriteIndex, consumer ReadIndex, interrupt masking and flow-control fields; the
// following full pages hold circular packet data. Indexes are data-relative byte
// offsets aligned to eight. Each packet has a sixteen-byte descriptor whose
// payload offset and total length use eight-byte units, plus an eight-byte trailer
// outside that length. The trailer records the preceding write position.
//
// A producer leaves spare space so equal indexes always mean empty, writes bytes
// before publishing its new index, and requests notification when an unmasked
// empty ring gains data. A consumer copies a complete packet before releasing its
// bytes. The optional pending-send-size feature records how much space a blocked
// writer needs and requests a wakeup only when a read crosses that threshold.
// This class reports notification decisions; ArmVmbus/SynIC perform actual delivery.
//
// IMPLEMENTATION NOTE: All guest virtual processors must be stopped. GPADL owns
// RAM translation, page/range validation and channel ownership. Ring creation
// rejects aliased/partial pages, captures a host generation and rejects further
// operations after teardown/reuse. Copies handle circular wrap across scattered
// pages; producer input is snapshotted before writes to support RAM-aliasing
// callers. Packet types, channel semantics and trailer validation are outside this
// helper; malformed bounds/indexes return Invalid. Header/payload memory ordering
// is explicit, but these fences do not permit concurrent guest execution.
// Primary wire/algorithm references: Linux hv_ring_buffer/vmpacket_descriptor and
// drivers/hv/ring_buffer.c. No independent complete VMBus specification is assumed.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L114
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L357
// https://github.com/torvalds/linux/blob/master/drivers/hv/ring_buffer.c#L275
// https://github.com/torvalds/linux/blob/master/drivers/hv/ring_buffer.c#L571

#include "arm_vmbus_ring.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
namespace pc {
namespace {
// Ring header occupies one Hyper-V 4 KiB page; indexes count bytes from data start,
// not from the header. Header fields below are little-endian u32 words.
// Linux hv_ring_buffer (feature_bits follows twelve reserved u32 words).
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L114
constexpr uint32_t HeaderPageBytes = ArmGpadl::GuestPageBytes;
enum HeaderField : unsigned {
    WriteIndex = 0,
    ReadIndex = 4,
    InterruptMask = 8,
    PendingSendBytes = 12,
    FeatureBits = 64
};
constexpr size_t HeaderFieldsBytes = HeaderField::FeatureBits + sizeof(uint32_t);
// Feature bit 0 advertises pending-send-size flow control: wake a blocked writer
// only when newly freed bytes cross its requested size, leaving one byte spare.
constexpr uint32_t PendingSendSizeFeature = 1;
// vmpacket_descriptor uses eight-byte units for payload offset and packet length;
// the packet's sixteen-byte descriptor is included in that length, trailer is not.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L357
enum PacketField : unsigned { PayloadOffset8 = 2, Length8 = 4 };
constexpr unsigned PacketDescriptorBytes = 16, PacketAlignmentBytes = 8,
                   PacketAlignmentMask = PacketAlignmentBytes - 1;
// A packet ends with a u64 previous-index trailer: high u32 old write index,
// low u32 zero. Consumers here skip the trailer; they do not validate its contents.
// https://github.com/torvalds/linux/blob/master/drivers/hv/ring_buffer.c#L97
constexpr unsigned PacketTrailerBytes = 8, PreviousIndexBytes = 4;
// Read a bounded little-endian packet/header field from validated storage.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L114
uint32_t read_little_endian(std::span<const uint8_t> b, unsigned at,
                            unsigned count = sizeof(uint32_t)) {
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= uint32_t(b[at + i]) << (8 * i);
    return value;
}
} // namespace
// Bind a validated ring window and capture its mapping generation; throw on invalid mappings.
// IMPLEMENTATION NOTE: All guest VPs stopped; GPADL prevents physical aliases and stale leases.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L114
ArmVmbusRing::ArmVmbusRing(ArmGpadl &m, uint32_t ch, uint32_t handle, uint64_t start,
                           uint32_t bytes)
    : memory(m), channel(ch), id(handle),
      capacity(bytes >= HeaderPageBytes ? bytes - HeaderPageBytes : 0), offset(start), lease(0) {
    if (!memory.valid_ring_mapping(ch, handle, start, bytes))
        throw std::runtime_error("Invalid VMBus ring mapping");
    lease = *memory.mapping_generation(ch, handle);
}
// Read the required header fields and validate aligned data-relative indexes/current host lease.
// IMPLEMENTATION NOTE: Reject indexes outside capacity rather than allowing host buffer overrun.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L114
bool ArmVmbusRing::read_header(Header &out) const {
    if (memory.mapping_generation(channel, id) != lease)
        return false;
    std::array<uint8_t, HeaderFieldsBytes> bytes{};
    if (!memory.read_mapped_bytes(channel, id, offset, bytes))
        return false;
    out = {read_little_endian(bytes, HeaderField::WriteIndex),
           read_little_endian(bytes, HeaderField::ReadIndex),
           read_little_endian(bytes, HeaderField::InterruptMask),
           read_little_endian(bytes, HeaderField::PendingSendBytes),
           read_little_endian(bytes, HeaderField::FeatureBits)};
    return out.write < capacity && out.read < capacity && !(out.write & PacketAlignmentMask) &&
           !(out.read & PacketAlignmentMask);
}
// Copy circular data through at most two GPADL reads, splitting at the data-area wrap.
// Caller has already bounded the complete transfer to the ring capacity.
// https://github.com/torvalds/linux/blob/master/drivers/hv/ring_buffer.c#L105
bool ArmVmbusRing::read_ring_data(uint32_t at, std::span<uint8_t> bytes) const {
    const auto first = std::min<size_t>(bytes.size(), capacity - at);
    return memory.read_mapped_bytes(channel, id, offset + HeaderPageBytes + at,
                                    bytes.first(first)) &&
           memory.read_mapped_bytes(channel, id, offset + HeaderPageBytes, bytes.subspan(first));
}
// Store circular data through at most two GPADL writes, splitting at the data-area wrap.
// Caller has already bounded the complete transfer to the ring capacity.
// https://github.com/torvalds/linux/blob/master/drivers/hv/ring_buffer.c#L105
bool ArmVmbusRing::write_ring_data(uint32_t at, std::span<const uint8_t> bytes) {
    const auto first = std::min<size_t>(bytes.size(), capacity - at);
    return memory.write_mapped_bytes(channel, id, offset + HeaderPageBytes + at,
                                     bytes.first(first)) &&
           memory.write_mapped_bytes(channel, id, offset + HeaderPageBytes, bytes.subspan(first));
}
// Publish one little-endian u32 ring-header field through its validated GPADL mapping.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L114
bool ArmVmbusRing::write_header_field(unsigned at, uint32_t value) {
    std::array<uint8_t, sizeof(uint32_t)> bytes{};
    for (unsigned i = 0; i < sizeof(uint32_t); ++i)
        bytes[i] = uint8_t(value >> (8 * i));
    return memory.write_mapped_bytes(channel, id, offset + at, bytes);
}
// Check eight-byte packet alignment and descriptor payload offset/total length; type is left to
// channels. https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L357
bool ArmVmbusRing::valid_packet(std::span<const uint8_t> packet) {
    if (packet.size() < PacketDescriptorBytes || (packet.size() & PacketAlignmentMask))
        return false;
    const auto dataOffset =
                   read_little_endian(packet, PacketField::PayloadOffset8, sizeof(uint16_t)) *
                   PacketAlignmentBytes,
               length = read_little_endian(packet, PacketField::Length8, sizeof(uint16_t)) *
                        PacketAlignmentBytes;
    return dataOffset >= PacketDescriptorBytes && dataOffset <= length && length == packet.size();
}
// Validate space, snapshot input, write packet/trailer, then publish WriteIndex with release
// ordering. Return Full without consuming input; notify only on an unmasked empty-to-nonempty
// transition. Linux hv_ringbuffer_write; host pending-size publication follows the shared
// flow-control contract.
// https://github.com/torvalds/linux/blob/master/drivers/hv/ring_buffer.c#L275
ArmVmbusRing::Result ArmVmbusRing::write_packet(std::span<const uint8_t> packet, bool &notify) {
    notify = false;
    Header h;
    if (!read_header(h) || !valid_packet(packet) || packet.size() + PacketTrailerBytes >= capacity)
        return Result::Invalid;
    const auto used = h.write >= h.read ? h.write - h.read : capacity - h.read + h.write;
    const auto total = uint32_t(packet.size() + PacketTrailerBytes);
    // Equal indexes mean empty: never fill all nominal free bytes or a full
    // ring would become indistinguishable from an empty ring on the next read.
    if (capacity - used <= total) {
        if ((h.features & PendingSendSizeFeature) &&
            !write_header_field(HeaderField::PendingSendBytes, total))
            return Result::Invalid;
        return Result::Full;
    }
    // Snapshot caller memory before any ring write, including aliases of RAM.
    std::vector<uint8_t> bytes(packet.begin(), packet.end());
    // The trailer is the old write index in the high half, low half reserved.
    bytes.resize(total);
    for (unsigned i = 0; i < sizeof(uint32_t); ++i)
        bytes[total - PreviousIndexBytes + i] = uint8_t(h.write >> (8 * i));
    if (!write_ring_data(h.write, bytes))
        return Result::Invalid;
    std::atomic_thread_fence(std::memory_order_release);
    if (!write_header_field(HeaderField::WriteIndex, (h.write + uint64_t(total)) % capacity))
        return Result::Invalid;
    if ((h.features & PendingSendSizeFeature) &&
        !write_header_field(HeaderField::PendingSendBytes, 0))
        return Result::Invalid;
    notify = !h.mask && !used;
    return Result::Ok;
}
// Copy one complete valid packet, publish ReadIndex, and report a blocked-writer space transition.
// Empty/invalid input leaves the caller's packet unchanged. Trailer bytes are skipped, not
// returned. Linux hv_pkt_iter_close, pending_send_sz threshold logic.
// https://github.com/torvalds/linux/blob/master/drivers/hv/ring_buffer.c#L571
ArmVmbusRing::Result ArmVmbusRing::read_packet(std::vector<uint8_t> &packet, bool &notify) {
    notify = false;
    Header h;
    if (!read_header(h))
        return Result::Invalid;
    const auto used = h.write >= h.read ? h.write - h.read : capacity - h.read + h.write;
    if (!used)
        return Result::Empty;
    if (used < PacketDescriptorBytes + PacketTrailerBytes)
        return Result::Invalid;
    std::atomic_thread_fence(std::memory_order_acquire);
    std::array<uint8_t, PacketDescriptorBytes> descriptor{};
    if (!read_ring_data(h.read, descriptor))
        return Result::Invalid;
    const auto length = read_little_endian(descriptor, PacketField::Length8, sizeof(uint16_t)) *
                        PacketAlignmentBytes;
    if (length < PacketDescriptorBytes || length + PacketTrailerBytes > used)
        return Result::Invalid;
    std::vector<uint8_t> bytes(length);
    if (!read_ring_data(h.read, bytes) || !valid_packet(bytes))
        return Result::Invalid;
    std::atomic_thread_fence(std::memory_order_release);
    if (!write_header_field(HeaderField::ReadIndex,
                            (h.read + uint64_t(length) + PacketTrailerBytes) % capacity))
        return Result::Invalid;
    const auto before = capacity - used, after = before + length + PacketTrailerBytes;
    // pending_send_sz is a strict threshold: exactly enough nominal free bytes
    // is still full. Wake only when moving from <= requested bytes to > them.
    notify = (h.features & PendingSendSizeFeature) && h.pending && before <= h.pending &&
             after > h.pending;
    packet = std::move(bytes);
    return Result::Ok;
}
} // namespace pc
