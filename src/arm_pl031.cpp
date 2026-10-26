#include "arm_pl031.hpp"
#include <chrono>
#include <utility>
namespace pc {
namespace {
// SPEC: ARM DDI 0224C, section 3.2, Table 3-1 (register offsets).
constexpr unsigned RTCDR = 0x00;
constexpr unsigned RTCMR = 0x04;
constexpr unsigned RTCLR = 0x08;
constexpr unsigned RTCCR = 0x0c;
constexpr unsigned RTCIMSC = 0x10;
constexpr unsigned RTCRIS = 0x14;
constexpr unsigned RTCMIS = 0x18;
constexpr unsigned RTCICR = 0x1c;
constexpr unsigned RTCPeriphID0 = 0xfe0;
constexpr unsigned RTCPCellID0 = 0xff0;
constexpr unsigned RTCPeriphID1 = 0xfe4;
constexpr unsigned RTCPCellID1 = 0xff4;
constexpr unsigned RTCPeriphID2 = 0xfe8;
constexpr unsigned RTCPCellID2 = 0xff8;
constexpr unsigned RTCPeriphID3 = 0xfec;
constexpr unsigned RTCPCellID3 = 0xffc;
constexpr uint32_t INTERRUPT_BIT = 1u << 0;
constexpr uint64_t COUNTER_PERIOD_SECONDS = 1ull << 32;
} // namespace

ArmPl031::ArmPl031(std::function<int64_t()> clock) : host_seconds(std::move(clock)) {
    if (!host_seconds)
        host_seconds = [] {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
    previous_counter = host_seconds();
}
int64_t ArmPl031::update_counter() {
    const int64_t current = host_seconds() + clock_adjustment;
    // SPEC: ARM DDI 0224C, sections 2.2.6 and 3.3.2 (match interrupt).
    // AD-HOC: Detect a crossed match between host samples, including counter wrap.
    // A backwards host clock step must not look like a 32-bit counter wrap.
    if (current > previous_counter) {
        const uint64_t elapsed = uint64_t(current - previous_counter);
        const uint32_t distance = match_register - uint32_t(previous_counter);
        if (elapsed >= COUNTER_PERIOD_SECONDS || (distance && distance <= elapsed))
            raw_interrupt = true;
    }
    previous_counter = current;
    return current;
}
// SPEC: ARM DDI 0224C, section 3.6 (RTCINTR).
bool ArmPl031::irq() {
    // Raw status is sampled on reads/unmask. A masked alarm needs no host poll.
    if (!interrupt_enabled)
        return false;
    update_counter();
    return raw_interrupt;
}
uint32_t ArmPl031::read(unsigned offset) {
    auto current = update_counter();
    // SPEC: ARM DDI 0224C, sections 3.3.1-3.3.7 (general register reads).
    switch (offset) {
    case RTCDR:
        return uint32_t(current);
    case RTCMR:
        return match_register;
    case RTCLR:
        return load_register;
    case RTCCR:
        // FIXME: ARM DDI 0224C, section 3.3.4: Implement RTCCR enable/reset behavior.
        // AD-HOC: Preserve the existing always-running clock until compatibility is tested.
        return 1;
    case RTCIMSC:
        return interrupt_enabled;
    case RTCRIS:
        return raw_interrupt;
    case RTCMIS:
        return interrupt_enabled && raw_interrupt;
    // SPEC: ARM DDI 0224C, sections 3.4 and 3.5 (identification bytes).
    case RTCPeriphID0:
        return 0x31;
    case RTCPeriphID1:
        return 0x10;
    case RTCPeriphID2:
        return 0x04;
    case RTCPeriphID3:
        return 0x00;
    case RTCPCellID0:
        return 0x0d;
    case RTCPCellID1:
        return 0xf0;
    case RTCPCellID2:
        return 0x05;
    case RTCPCellID3:
        return 0xb1;
    // AD-HOC: Unimplemented and reserved registers read as zero.
    default:
        return 0;
    }
}
void ArmPl031::write(unsigned offset, uint32_t value) {
    update_counter();
    // SPEC: ARM DDI 0224C, sections 3.3.2, 3.3.3, 3.3.5 and 3.3.8.
    switch (offset) {
    case RTCMR:
        match_register = value;
        if (match_register == uint32_t(previous_counter))
            raw_interrupt = true;
        break;
    case RTCLR:
        load_register = value;
        clock_adjustment = int64_t(value) - host_seconds();
        previous_counter = value;
        if (match_register == value)
            raw_interrupt = true;
        break;
    case RTCIMSC:
        interrupt_enabled = value & INTERRUPT_BIT;
        break;
    case RTCICR:
        if (value & INTERRUPT_BIT)
            raw_interrupt = false;
        break;
    // FIXME: ARM DDI 0224C, chapter 4: Implement integration test registers.
    // AD-HOC: Unsupported writes, including RTCCR, are ignored.
    default:
        break;
    }
}
} // namespace pc
