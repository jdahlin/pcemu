#pragma once
#include <array>
#include <cstdint>
namespace pc {
struct PIT8254 {
    struct Counter {
        uint32_t reload = 65536;
        uint64_t loadedAt = 0, lastEdge = 0, pausedAt = 0;
        uint16_t writeValue = 0, latched = 0;
        uint8_t rw = 3, mode = 3, status = 0;
        bool writeHigh = false, readHigh = false, countLatched = false, latchHigh = false,
             statusLatched = false, gate = true, nullCount = false, bcd = false;
        uint16_t value(uint64_t now) const;
        bool output(uint64_t now) const;
        void latch(uint64_t now);
        void latchStatus(uint64_t now);
        void load(uint16_t raw, uint64_t now);
        uint8_t read(uint64_t now);
        void write(uint8_t v, uint64_t now);
        void setGate(bool v, uint64_t now);
        bool edge(uint64_t now);
        bool eventDue(uint64_t now) const {
            if (nullCount || !gate)
                return false;
            auto elapsed = now - loadedAt;
            if (mode == 2 || mode == 3)
                return elapsed >= (lastEdge + 1) * reload;
            return (elapsed >= reload ? 1u : 0u) != lastEdge;
        }
    };
    std::array<Counter, 3> counters{};
    PIT8254();
    uint8_t read(unsigned channel, uint64_t now) {
        return counters[channel % 3].read(now);
    }
    void write(unsigned port, uint8_t value, uint64_t now);
    bool tick(uint64_t now) {
        return counters[0].eventDue(now) && counters[0].edge(now);
    }
};
} // namespace pc
