#pragma once
#include "arm_gpadl.hpp"
#include <span>
#include <vector>
namespace pc {
// One VMBus ring: a 4 KiB header followed by a page-aligned circular byte area.
// Caller stops every VP. Teardown/reuse invalidates existing ring objects.
class ArmVmbusRing {
  public:
    enum class Result { Ok, Empty, Full, Invalid };
    ArmVmbusRing(ArmGpadl &memory, uint32_t channel, uint32_t id, uint64_t offset, uint32_t bytes);
    // Wire packet includes the 16-byte descriptor and 8-byte payload padding,
    // but excludes the ring trailer. Notifications are returned to the caller.
    Result push(std::span<const uint8_t> packet, bool &notify);
    Result pop(std::vector<uint8_t> &packet, bool &notify);
  private:
    ArmGpadl &memory;
    uint32_t channel, id, capacity;
    uint64_t offset, lease;
    struct Header { uint32_t write, read, mask, pending, features; };
    bool header(Header &out) const;
    bool copy(uint32_t at, std::span<uint8_t> bytes) const;
    bool store(uint32_t at, std::span<const uint8_t> bytes);
    bool field(unsigned at, uint32_t value);
    static bool validPacket(std::span<const uint8_t> packet);
};
}
