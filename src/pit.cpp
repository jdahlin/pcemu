// The Intel 8254 programmable interval timer (PIT) provides three independent
// down-counters. Software selects a mode and count through a Control Word and
// byte-sized counter ports. OUT is an output signal driven by the counting mode;
// GATE can pause counting or trigger/retrigger a sequence. Count and status latches
// let software read stable snapshots while time continues to advance.
// Binary counting uses sixteen bits; binary-coded decimal (BCD) stores each
// decimal digit in four bits. A programmed zero represents the maximum period.
// PC/AT wiring uses counter 0 for timer interrupts, counter 1 for refresh status,
// and counter 2 for the speaker. Machine wiring and sound presentation are external.
//
// IMPLEMENTATION NOTE: now is a caller-supplied tick count, not host wall time.
// This PC supplies CPU instruction count divided by four. Whole periods skipped
// between polls coalesce into one pending event; this is not cycle-accurate timing.
// COMPATIBILITY: Initial modes, refresh divisor and speaker gate match existing
// firmware assumptions; physical 8254 power-up state is undefined.
// FIXME: Model next-CLK count transfers, deferred periodic reloads and trigger-only
// arming precisely before claiming cycle-accurate mode behavior.
// Reference: Intel 8254 datasheet, 231164-005, September 1993.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=5

#include "pit.hpp"
namespace pc {
namespace {
// Control Word/Read-Back/Status formats, Figures 7, 10 and 11.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=6
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=8
// SC1:SC0 occupy control-word bits 7:6.
constexpr unsigned CounterSelectShift = 6;
// RW1:RW0 occupy bits 5:4 in control and status bytes.
constexpr unsigned ReadWriteModeShift = 4;
// Two bits select latch/low/high/low-then-high access.
constexpr unsigned ReadWriteModeMask = 3;
// M2:M0 occupy control and status bits 3:1.
constexpr unsigned CountingModeShift = 1;
// Three bits encode the six modes and mode-2/3 aliases.
constexpr unsigned CountingModeMask = 7;
// M2 is ignored for mode 2/3; clear it to normalize encodings 6/7.
constexpr unsigned ModeAliasBit = 4;
// BCD bit 0 selects four-decade decimal rather than binary counting.
constexpr unsigned BinaryCodedDecimalBit = 1;
// Read-back COUNT bit 5 is active-low: zero requests a count latch.
constexpr unsigned ReadBackCountNotRequested = 32;
// Read-back STATUS bit 4 is active-low: zero requests a status latch.
constexpr unsigned ReadBackStatusNotRequested = 16;
// Read-back CNT0 is bit 1; shift it for CNT1/CNT2 selection.
constexpr unsigned ReadBackCounter0Select = 2;
// Status D7 reports the OUT pin level.
constexpr unsigned OutputStatusBit = 128;
// Status D6 reports that no new count has been loaded.
constexpr unsigned NullCountStatusBit = 64;
// Count transfers use eight-bit data ports, low byte before high byte.
constexpr unsigned CountByteBits = 8;
// A BCD digit occupies one four-bit nibble.
constexpr unsigned DecimalDigitMask = 15;
// The running binary count wraps within sixteen bits.
constexpr unsigned BinaryCountMask = 65535;
// COMPATIBILITY: Initial model ticks per memory-refresh period.
constexpr unsigned InitialRefreshDivisor = 18;
} // namespace

// Encode a decimal value as four BCD digits, with zero representing a full period.
// Intel 8254, Control Word Format, BCD.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=6
static uint16_t encode_binary_coded_decimal(uint32_t v) {
    v %= PIT8254::DecimalCountPeriod;
    return (v / 1000 << 12) | ((v / 100) % 10 << 8) | ((v / 10) % 10 << 4) | (v % 10);
}
// Compute the current count, accounting for GATE pauses and mode-3 double decrement.
// Intel 8254, Mode Definitions (modes 0-3).
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=10
uint16_t PIT8254::Counter::counter_value(uint64_t now) const {
    if (!gate && (mode == Mode::InterruptOnTerminalCount || mode == Mode::RateGenerator ||
                  mode == Mode::SquareWave || mode == Mode::SoftwareTriggeredStrobe))
        now = pausedAt;
    uint64_t elapsed = now - loadedAt;
    uint32_t result;
    if (mode == Mode::RateGenerator)
        result = reload - uint32_t(elapsed % reload);
    else if (mode == Mode::SquareWave) {
        // Mode 3 decrements by two; odd divisors give OUT one extra high tick.
        // https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=13
        unsigned phase = elapsed % reload, high = (reload + 1) / 2;
        if (phase < high)
            result = (reload & 1) ? (phase ? reload + 1 - 2 * phase : reload) : reload - 2 * phase;
        else {
            phase -= high;
            result = (reload & 1) ? reload - 1 - 2 * phase : reload - 2 * phase;
        }
    } else
        result = (reload - uint32_t(elapsed)) & BinaryCountMask;
    return bcd ? encode_binary_coded_decimal(result) : uint16_t(result);
}
// Sample OUT for the programmed mode, including terminal count and strobe pulses.
// Intel 8254, Mode Definitions (modes 0-5).
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=10
bool PIT8254::Counter::output_high(uint64_t now) const {
    if (nullCount)
        return mode != Mode::InterruptOnTerminalCount;
    if (!gate && (mode == Mode::RateGenerator || mode == Mode::SquareWave))
        return true;
    uint64_t elapsed = now - loadedAt;
    if (!gate && (mode == Mode::InterruptOnTerminalCount || mode == Mode::SoftwareTriggeredStrobe))
        elapsed = pausedAt - loadedAt;
    if (mode == Mode::InterruptOnTerminalCount || mode == Mode::HardwareRetriggerableOneShot)
        return elapsed >= reload;
    if (mode == Mode::RateGenerator)
        return elapsed % reload != reload - 1;
    if (mode == Mode::SquareWave)
        return elapsed % reload < (reload + 1) / 2;
    // Modes 4 and 5 strobe low for one model tick at terminal count.
    // https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=15
    // https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=16
    return elapsed != reload;
}
// Decode a completed Count Register value and restart this model counting interval.
// Intel 8254, Write Operations and Mode Definitions.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=6
void PIT8254::Counter::load_count(uint16_t raw, uint64_t now) {
    reload = bcd ? ((raw >> 12) * 1000 + ((raw >> 8) & DecimalDigitMask) * 100 +
                    ((raw >> 4) & DecimalDigitMask) * 10 + (raw & DecimalDigitMask))
                 : raw;
    if (!reload)
        reload = bcd ? DecimalCountPeriod : BinaryCountPeriod;
    loadedAt = pausedAt = now;
    lastEdge = 0;
    nullCount = false;
    readHigh = false;
}
// Capture a count snapshot only if no previous count latch remains unread.
// Intel 8254, Counter Latch Command.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=7
void PIT8254::Counter::latch_count(uint64_t now) {
    if (!countLatched) {
        latched = counter_value(now);
        countLatched = true;
        latchHigh = false;
    }
}
// Capture OUT, null-count and Control Word fields without replacing unread status.
// Intel 8254, Read-Back Command, Figure 11 Status Byte.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=8
void PIT8254::Counter::latch_status(uint64_t now) {
    if (!statusLatched) {
        status = (output_high(now) ? OutputStatusBit : 0) | (nullCount ? NullCountStatusBit : 0) |
                 (rw << ReadWriteModeShift) | (mode << CountingModeShift) |
                 (bcd ? BinaryCodedDecimalBit : 0);
        statusLatched = true;
    }
}
// Consume a latched status byte first, then the configured count byte sequence.
// Intel 8254, Read-Back Command and Read Operations.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=9
uint8_t PIT8254::Counter::read_counter(uint64_t now) {
    if (statusLatched) {
        statusLatched = false;
        return status;
    }
    auto v = countLatched ? latched : counter_value(now);
    bool high = rw == ReadWriteMode::MostSignificantByte ||
                (rw == ReadWriteMode::LeastThenMostSignificantByte &&
                 (countLatched ? latchHigh : readHigh));
    uint8_t result = high ? v >> CountByteBits : v;
    if (countLatched) {
        if (rw != ReadWriteMode::LeastThenMostSignificantByte || latchHigh)
            countLatched = false;
        else
            latchHigh = true;
    } else if (rw == ReadWriteMode::LeastThenMostSignificantByte)
        readHigh = !readHigh;
    return result;
}
// Assemble count bytes according to RW1:RW0 and load the completed value.
// Intel 8254, Control Word Format and Write Operations.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=6
void PIT8254::Counter::write_counter(uint8_t v, uint64_t now) {
    if (rw == ReadWriteMode::LeastSignificantByte)
        load_count(v, now);
    else if (rw == ReadWriteMode::MostSignificantByte)
        load_count(uint16_t(v) << CountByteBits, now);
    else if (!writeHigh) {
        writeValue = v;
        writeHigh = true;
    } else {
        load_count(writeValue | (uint16_t(v) << CountByteBits), now);
        writeHigh = false;
    }
}
// Apply a GATE transition, pausing or retriggering the selected counting mode.
// Intel 8254, Mode Definitions (GATE behavior).
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=10
void PIT8254::Counter::set_gate(bool v, uint64_t now) {
    if (gate == v)
        return;
    if (!v)
        pausedAt = now;
    else if (mode == Mode::HardwareRetriggerableOneShot || mode == Mode::RateGenerator ||
             mode == Mode::SquareWave || mode == Mode::HardwareTriggeredStrobe) {
        loadedAt = now;
        lastEdge = 0;
    } else
        loadedAt += now - pausedAt;
    gate = v;
}
// Consume newly crossed terminal counts, reporting at most one event per poll.
// Intel 8254, Mode Definitions (terminal count and periodic modes).
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=10
bool PIT8254::Counter::consume_terminal_count_event(uint64_t now) {
    if (!terminal_count_event_due(now))
        return false;
    // IMPLEMENTATION NOTE: Edge bookkeeping follows elapsed periods, not pin simulation.
    uint64_t elapsed = now - loadedAt;
    uint64_t edges = (mode == Mode::RateGenerator || mode == Mode::SquareWave) ? elapsed / reload
                     : elapsed >= reload                                       ? 1
                                                                               : 0;
    bool raised = edges > lastEdge;
    lastEdge = edges;
    return raised;
}
// Initialize board-compatible modes, refresh count and speaker gating.
// Intel 8254, Operational Description, General (undefined power-up state).
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=5
PIT8254::PIT8254() {
    counters[TimerInterruptCounter].mode = Mode::SquareWave;
    counters[RefreshCounter].mode = Mode::RateGenerator;
    counters[RefreshCounter].reload = InitialRefreshDivisor;
    counters[SpeakerCounter].gate = false;
}
// Decode a counter data, Control Word or Read-Back write at a relative port.
// Intel 8254, Programming the 8254 and Read-Back Command.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=6
void PIT8254::write_register(unsigned port, uint8_t v, uint64_t now) {
    if (port < CounterCount) {
        counters[port].write_counter(v, now);
        return;
    }
    unsigned channel = v >> CounterSelectShift;
    if (channel == ControlWordRegister) {
        for (unsigned i = 0; i < CounterCount; i++)
            if (v & (ReadBackCounter0Select << i)) {
                if (!(v & ReadBackCountNotRequested))
                    counters[i].latch_count(now);
                if (!(v & ReadBackStatusNotRequested))
                    counters[i].latch_status(now);
            }
        return;
    }
    auto &c = counters[channel];
    unsigned rw = (v >> ReadWriteModeShift) & ReadWriteModeMask;
    if (rw == ReadWriteMode::CounterLatchCommand) {
        c.latch_count(now);
        return;
    }
    c.rw = rw;
    c.mode = (v >> CountingModeShift) & CountingModeMask;
    if (c.mode > Mode::HardwareTriggeredStrobe)
        c.mode -= ModeAliasBit;
    c.bcd = v & BinaryCodedDecimalBit;
    c.nullCount = true;
    c.writeHigh = c.readHigh = c.countLatched = c.statusLatched = false;
    c.loadedAt = now;
}
} // namespace pc
