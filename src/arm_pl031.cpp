// The ARM PrimeCell PL031 is a real-time clock (RTC) with one programmable alarm.
// Software reads time as seconds, sets a new time, and requests an interrupt at
// an alarm time. Calendar fields such as year, month and day are handled by the OS.
//
// The device has four cooperating parts:
// - Timekeeping: a 32-bit counter advances once per second and wraps to zero.
//   The Update block applies a load-register offset to obtain the RTC value.
// - Alarm: the Match Register holds the target time. Reaching it latches raw
//   interrupt status, which remains set until software clears it.
// - Interrupt control: a mask gates that status onto RTCINTR. A high signal means
//   an interrupt is requested. Masking suppresses the output without clearing it.
// - Register interface: memory-mapped registers expose these controls and
//   read-only identification bytes. Offsets are relative to the device base.
//
// The machine supplies that base address and connects RTCINTR to its interrupt
// controller. This model samples an injectable seconds source rather than clock
// wires; the default is host wall time, including sleep. Guest time changes affect
// only the device offset. Alarm checks include time elapsed between host samples.
//
// Reference: ARM PrimeCell Real Time Clock (PL031) Technical Reference Manual,
// ARM DDI 0224C, r1p3, chapters 2 (operation) and 3 (programmers model).
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=14
// AD-HOC: The clock starts at host time and remains enabled. RTCCR and test-mode
// support are incomplete; the relevant FIXME comments identify these gaps.

#include "arm_pl031.hpp"
#include <chrono>
#include <utility>
namespace pc {
namespace {
// ARM DDI 0224C, 3.2 Summary of RTC registers, Table 3-1.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=23
// Data Register: reads the current RTC seconds value.
constexpr unsigned RTCDR = 0x00;
// Match Register: stores the alarm time to compare against the RTC value.
constexpr unsigned RTCMR = 0x04;
// Load Register: writes set the RTC time; reads return the last written value.
constexpr unsigned RTCLR = 0x08;
// Control Register: controls startup; this model always reports enabled.
constexpr unsigned RTCCR = 0x0c;
// Interrupt Mask Set or Clear: bit 0 enables the external interrupt output.
constexpr unsigned RTCIMSC = 0x10;
// Raw Interrupt Status: reports a latched alarm even when its output is masked.
constexpr unsigned RTCRIS = 0x14;
// Masked Interrupt Status: reports an alarm only when its output is enabled.
constexpr unsigned RTCMIS = 0x18;
// Interrupt Clear Register: writing bit 0 acknowledges a latched alarm.
constexpr unsigned RTCICR = 0x1c;
// Peripheral ID byte 0: low eight bits of the PL031 part number (0x031).
constexpr unsigned RTCPeriphID0 = 0xfe0;
// PrimeCell ID byte 0: bits 7:0 of the standard signature 0xb105f00d.
constexpr unsigned RTCPCellID0 = 0xff0;
// Peripheral ID byte 1: remaining part-number bits and low ARM designer-ID bits.
constexpr unsigned RTCPeriphID1 = 0xfe4;
// PrimeCell ID byte 1: bits 15:8 of the standard PrimeCell signature.
constexpr unsigned RTCPCellID1 = 0xff4;
// Peripheral ID byte 2: hardware revision and remaining designer-ID bits.
constexpr unsigned RTCPeriphID2 = 0xfe8;
// PrimeCell ID byte 2: bits 23:16 of the standard PrimeCell signature.
constexpr unsigned RTCPCellID2 = 0xff8;
// Peripheral ID byte 3: hardware configuration identifier.
constexpr unsigned RTCPeriphID3 = 0xfec;
// PrimeCell ID byte 3: bits 31:24 of the standard PrimeCell signature.
constexpr unsigned RTCPCellID3 = 0xffc;
// Only bit 0 participates in interrupt mask, status and clear registers.
constexpr uint32_t INTERRUPT_BIT = 1u << 0;
// A 32-bit seconds value repeats after 2^32 seconds; longer gaps cross every alarm.
constexpr uint64_t COUNTER_PERIOD_SECONDS = 1ull << 32;
} // namespace

// Create the emulated RTC with an injectable seconds source.
// ARM DDI 0224C, 2.3.2 Clock signals.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=20
// AD-HOC: Start from host wall time rather than the hardware reset value.
ArmPl031::ArmPl031(std::function<int64_t()> clock) : host_seconds(std::move(clock)) {
    if (!host_seconds)
        host_seconds = [] {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
    previous_counter = host_seconds();
}
// Update the RTC value and latch a match interrupt between samples.
// ARM DDI 0224C, 2.2.4 Update block and 2.2.6 Counter block.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=17
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=18
int64_t ArmPl031::update_rtc_value() {
    const int64_t current = host_seconds() + clock_adjustment;
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
// Return whether the active-high RTCINTR output is asserted.
// ARM DDI 0224C, 3.6 Interrupts.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=31
bool ArmPl031::rtcintr_asserted() {
    // Raw status is sampled on reads/unmask. A masked alarm needs no host poll.
    if (!interrupt_enabled)
        return false;
    update_rtc_value();
    return raw_interrupt;
}
// Read the addressed RTC register, sampling the current RTC value first.
// ARM DDI 0224C, 3.2 Summary of RTC registers and 3.3 General registers.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=23
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=24
uint32_t ArmPl031::read_register(unsigned offset) {
    auto current = update_rtc_value();
    switch (offset) {
    case RTCDR:
        return uint32_t(current);
    case RTCMR:
        return match_register;
    case RTCLR:
        return load_register;
    case RTCCR:
        // https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=25
        // FIXME: ARM DDI 0224C, section 3.3.4: Implement RTCCR enable/reset behavior.
        // AD-HOC: Preserve the existing always-running clock until compatibility is tested.
        return 1;
    // ARM DDI 0224C, 3.3.5-3.3.7 (mask and interrupt status).
    // https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=25
    case RTCIMSC:
        return interrupt_enabled;
    case RTCRIS:
        return raw_interrupt;
    case RTCMIS:
        return interrupt_enabled && raw_interrupt;
    // ARM DDI 0224C, 3.4 Peripheral identification registers.
    // https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=27
    case RTCPeriphID0:
        return 0x31;
    case RTCPeriphID1:
        return 0x10;
    case RTCPeriphID2:
        return 0x04;
    case RTCPeriphID3:
        return 0x00;
    // ARM DDI 0224C, 3.5 PrimeCell identification registers.
    // https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=29
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
// Dispatch a register write after sampling the RTC value and pending match.
// ARM DDI 0224C, 3.2 Summary of RTC registers.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=23
void ArmPl031::write_register(unsigned offset, uint32_t value) {
    update_rtc_value();
    switch (offset) {
    case RTCMR:
        write_match_register(value);
        break;
    case RTCLR:
        write_load_register(value);
        break;
    case RTCIMSC:
        write_interrupt_mask(value);
        break;
    case RTCICR:
        clear_interrupt(value);
        break;
    case RTCCR:
        // https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=25
        // FIXME: Implement RTCCR enable/reset behavior (3.3.4).
        break;
    // https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=35
    // FIXME: Implement integration test registers (4.3).
    // AD-HOC: Unsupported writes are ignored.
    default:
        break;
    }
}

// Store RTCMR, the Match Register, and recognize a match at the current RTC value.
// ARM DDI 0224C, 3.3.2 Match Register, RTCMR.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=24
void ArmPl031::write_match_register(uint32_t value) {
    match_register = value;
    if (match_register == uint32_t(previous_counter))
        raw_interrupt = true;
}

// Store RTCLR and apply its update value to the RTC.
// ARM DDI 0224C, 3.3.3 Load Register, RTCLR, and 2.2.4 Update block.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=24
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=17
void ArmPl031::write_load_register(uint32_t value) {
    load_register = value;
    // AD-HOC: Apply the load immediately as an offset from host seconds.
    clock_adjustment = int64_t(value) - host_seconds();
    previous_counter = value;
    if (match_register == value)
        raw_interrupt = true;
}

// Set or clear RTCIMSC bit 0, which enables the RTCINTR output.
// ARM DDI 0224C, 3.3.5 Interrupt Mask Set or Clear register, RTCIMSC.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=25
void ArmPl031::write_interrupt_mask(uint32_t value) {
    interrupt_enabled = value & INTERRUPT_BIT;
}

// Clear the latched interrupt when RTCICR bit 0 is written as one.
// ARM DDI 0224C, 3.3.8 Interrupt Clear Register, RTCICR.
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e#page=26
void ArmPl031::clear_interrupt(uint32_t value) {
    if (value & INTERRUPT_BIT)
        raw_interrupt = false;
}
} // namespace pc
