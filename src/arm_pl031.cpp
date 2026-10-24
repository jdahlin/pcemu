#include "arm_pl031.hpp"
#include <chrono>
#include <utility>
namespace pc {
ArmPl031::ArmPl031(std::function<int64_t()> clock) : seconds(std::move(clock)) {
    if (!seconds) seconds = [] {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
    previous = seconds();
}
int64_t ArmPl031::update() {
    const int64_t current = seconds() + adjustment;
    // Latch an alarm even if the host does not poll on the exact matching second.
    // A backwards host clock step must not look like a 32-bit counter wrap.
    if (current > previous) {
        const uint64_t elapsed = uint64_t(current - previous);
        const uint32_t distance = match - uint32_t(previous);
        if (elapsed >= (1ull << 32) || (distance && distance <= elapsed)) pending = true;
    }
    previous = current;
    return current;
}
bool ArmPl031::irq() {
    // Raw status is sampled on reads/unmask. A masked alarm needs no host poll.
    if (!masked) return false;
    update();
    return pending;
}
uint32_t ArmPl031::read(unsigned offset) {
    auto current = update();
    switch (offset) {
    case 0x00: return uint32_t(current);
    case 0x04: return match;
    case 0x08: return loaded;
    case 0x0c: return 1; // Enabled at reset; enable is sticky on PL031.
    case 0x10: return masked;
    case 0x14: return pending;
    case 0x18: return masked && pending;
    case 0xfe0: return 0x31;
    case 0xfe4: return 0x10;
    case 0xfe8: return 0x04;
    case 0xfec: return 0x00;
    case 0xff0: return 0x0d;
    case 0xff4: return 0xf0;
    case 0xff8: return 0x05;
    case 0xffc: return 0xb1;
    default: return 0;
    }
}
void ArmPl031::write(unsigned offset, uint32_t value) {
    update();
    switch (offset) {
    case 0x04: match = value; if (match == uint32_t(previous)) pending = true; break;
    case 0x08:
        loaded = value;
        adjustment = int64_t(value) - seconds();
        previous = value;
        if (match == value) pending = true;
        break;
    case 0x10: masked = value & 1; break;
    case 0x1c: if (value & 1) pending = false; break;
    default: break;
    }
}
} // namespace pc
