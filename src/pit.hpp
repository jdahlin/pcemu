#pragma once
#include <array>
#include <cstdint>
namespace pc {
// Intel 8254 Programmable Interval Timer, 231164-005, September 1993.
// https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf
// Three independently programmed counters; see pit.cpp for timing/model boundaries.
struct PIT8254 {
    // Each of the three counter data ports has an independent Counting Element.
    static constexpr unsigned CounterCount = 3;
    // A binary count register value of zero represents a full 16-bit period.
    static constexpr uint32_t BinaryCountPeriod = 65536;
    // A zero BCD count represents four decimal decades (0000 through 9999).
    static constexpr uint32_t DecimalCountPeriod = 10000;
    // A1:A0 = 11 addresses the Control Word Register rather than a counter.
    static constexpr unsigned ControlWordRegister = 3;
    // PC/AT wiring uses counter 0 for periodic IRQ0 delivery.
    static constexpr unsigned TimerInterruptCounter = 0;
    // PC/AT wiring uses counter 1 for the legacy memory-refresh signal.
    static constexpr unsigned RefreshCounter = 1;
    // PC/AT wiring gates counter 2 through system port B for the speaker.
    static constexpr unsigned SpeakerCounter = 2;
    // Figure 7 Control Word Format, and Mode Definitions (names follow the manual).
    // https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=6
    enum Mode : uint8_t {
        // OUT rises when the countdown reaches zero.
        InterruptOnTerminalCount = 0,
        // GATE's rising edge triggers a retriggerable low output pulse.
        HardwareRetriggerableOneShot = 1,
        // Periodic divide-by-N output with a one-clock low pulse.
        RateGenerator = 2,
        // Periodic output with approximately equal high and low phases.
        SquareWave = 3,
        // Loading a count starts a countdown ending in a one-clock low pulse.
        SoftwareTriggeredStrobe = 4,
        // GATE triggers a retriggerable countdown ending in a low pulse.
        HardwareTriggeredStrobe = 5,
    };
    // RW1:RW0 in the Control Word; governs byte sequencing or count latching.
    enum ReadWriteMode : uint8_t {
        // Snapshot the running count without reprogramming the counter.
        CounterLatchCommand = 0,
        // Transfer bits 7:0 only; the high byte is zero on writes.
        LeastSignificantByte = 1,
        // Transfer bits 15:8 only; the low byte is zero on writes.
        MostSignificantByte = 2,
        // Transfer the low byte followed by the high byte.
        LeastThenMostSignificantByte = 3,
    };
    struct Counter {
        uint32_t reload = BinaryCountPeriod;
        uint64_t loadedAt = 0, lastEdge = 0, pausedAt = 0;
        uint16_t writeValue = 0, latched = 0;
        uint8_t rw = ReadWriteMode::LeastThenMostSignificantByte, mode = Mode::SquareWave,
                status = 0;
        bool writeHigh = false, readHigh = false, countLatched = false, latchHigh = false,
             statusLatched = false, gate = true, nullCount = false, bcd = false;
        // Return the current binary or BCD count.
        uint16_t counter_value(uint64_t now) const;
        // Sample the OUT pin level for the programmed mode.
        bool output_high(uint64_t now) const;
        // Capture an unread count without replacing an earlier latch.
        void latch_count(uint64_t now);
        // Capture mode, OUT and null-count status for read-back.
        void latch_status(uint64_t now);
        // Decode and load a completed count value at this model tick.
        void load_count(uint16_t raw, uint64_t now);
        // Read latched status/count or the running counter byte.
        uint8_t read_counter(uint64_t now);
        // Assemble the configured one-byte or two-byte count write.
        void write_counter(uint8_t v, uint64_t now);
        // Apply a GATE change, pausing or retriggering as the mode requires.
        void set_gate(bool v, uint64_t now);
        // Report a newly crossed terminal count, coalescing skipped periods.
        bool consume_terminal_count_event(uint64_t now);
        // Check whether terminal-count bookkeeping needs an update.
        // IMPLEMENTATION NOTE: Board scheduling around the counter model.
        // https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=10
        bool terminal_count_event_due(uint64_t now) const {
            if (nullCount || !gate)
                return false;
            auto elapsed = now - loadedAt;
            if (mode == Mode::RateGenerator || mode == Mode::SquareWave)
                return elapsed >= (lastEdge + 1) * reload;
            return (elapsed >= reload ? 1u : 0u) != lastEdge;
        }
    };
    std::array<Counter, CounterCount> counters{};
    // Initialize the existing PC/AT-compatible timer state.
    PIT8254();
    // Reset all counters at the supplied model tick.
    // IMPLEMENTATION NOTE: Board scheduling around the counter model.
    // https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=10
    void reset(uint64_t now) {
        *this = PIT8254{};
        for (auto &counter : counters)
            counter.loadedAt = now;
    }
    // Read latched status/count or the running counter byte.
    // IMPLEMENTATION NOTE: Board scheduling around the counter model.
    // https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=10
    uint8_t read_counter(unsigned channel, uint64_t now) {
        return counters[channel % CounterCount].read_counter(now);
    }
    // Decode a relative data/control port write.
    void write_register(unsigned port, uint8_t value, uint64_t now);
    // Consume a counter-0 timer event for the board interrupt input.
    // IMPLEMENTATION NOTE: Board scheduling around the counter model.
    // https://www.ecsdump.net/wp-content/uploads/2020/12/8254.pdf#page=10
    bool timer_interrupt_due(uint64_t now) {
        return counters[TimerInterruptCounter].terminal_count_event_due(now) &&
               counters[TimerInterruptCounter].consume_terminal_count_event(now);
    }
};
} // namespace pc
