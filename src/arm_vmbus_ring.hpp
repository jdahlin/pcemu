#pragma once
#include "arm_gpadl.hpp"
#include <span>
#include <vector>
namespace pc {
// VMBus shared ring wire reference: Linux hv_ring_buffer / vmpacket_descriptor.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L114
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L357
// One VMBus ring: a 4 KiB header followed by a page-aligned circular byte area.
// Caller stops every VP. Teardown/reuse invalidates existing ring objects.
class ArmVmbusRing {
  public:
    enum class Result { Ok, Empty, Full, Invalid };
    // Bind a validated page-aligned GPADL ring window and capture its host generation.
    ArmVmbusRing(ArmGpadl &memory, uint32_t channel, uint32_t id, uint64_t offset, uint32_t bytes);
    // Wire packet includes the 16-byte descriptor and 8-byte payload padding,
    // but excludes the ring trailer. Notifications are returned to the caller.
    Result write_packet(std::span<const uint8_t> packet, bool &notify);
    // Copy one validated packet, advance ReadIndex and report sender-space notification.
    Result read_packet(std::vector<uint8_t> &packet, bool &notify);

  private:
    ArmGpadl &memory;
    uint32_t channel, id, capacity;
    uint64_t offset, lease;
    struct Header {
        uint32_t write, read, mask, pending, features;
    };
    // Read/validate current ring indexes only while the GPADL generation still matches.
    bool read_header(Header &out) const;
    // Read circular data across at most one wrap via the owning GPADL.
    bool read_ring_data(uint32_t at, std::span<uint8_t> bytes) const;
    // Write circular data across at most one wrap via the owning GPADL.
    bool write_ring_data(uint32_t at, std::span<const uint8_t> bytes);
    // Store one little-endian 32-bit header field in the owning GPADL.
    bool write_header_field(unsigned at, uint32_t value);
    // Validate packet alignment and descriptor offset/length against the complete buffer.
    static bool valid_packet(std::span<const uint8_t> packet);
};
} // namespace pc
