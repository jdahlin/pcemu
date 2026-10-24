#include "pit.hpp"
namespace pc {
static uint16_t packBCD(uint32_t v) {
    v %= 10000;
    return (v / 1000 << 12) | ((v / 100) % 10 << 8) | ((v / 10) % 10 << 4) | (v % 10);
}
uint16_t PIT8254::Counter::value(uint64_t now) const {
    if (!gate && (mode == 0 || mode == 2 || mode == 3 || mode == 4))
        now = pausedAt;
    uint64_t elapsed = now - loadedAt;
    uint32_t result;
    if (mode == 2)
        result = reload - uint32_t(elapsed % reload);
    else if (mode == 3) {
        unsigned phase = elapsed % reload, high = (reload + 1) / 2;
        if (phase < high)
            result = (reload & 1) ? (phase ? reload + 1 - 2 * phase : reload) : reload - 2 * phase;
        else {
            phase -= high;
            result = (reload & 1) ? reload - 1 - 2 * phase : reload - 2 * phase;
        }
    } else
        result = (reload - uint32_t(elapsed)) & 65535;
    return bcd ? packBCD(result) : uint16_t(result);
}
bool PIT8254::Counter::output(uint64_t now) const {
    if (nullCount)
        return mode != 0;
    if (!gate && (mode == 2 || mode == 3))
        return true;
    uint64_t elapsed = now - loadedAt;
    if (!gate && (mode == 0 || mode == 4))
        elapsed = pausedAt - loadedAt;
    if (mode == 0 || mode == 1)
        return elapsed >= reload;
    if (mode == 2)
        return elapsed % reload != reload - 1;
    if (mode == 3)
        return elapsed % reload < (reload + 1) / 2;
    return elapsed != reload;
}
void PIT8254::Counter::load(uint16_t raw, uint64_t now) {
    reload =
        bcd ? ((raw >> 12) * 1000 + ((raw >> 8) & 15) * 100 + ((raw >> 4) & 15) * 10 + (raw & 15))
            : raw;
    if (!reload)
        reload = bcd ? 10000 : 65536;
    loadedAt = pausedAt = now;
    lastEdge = 0;
    nullCount = false;
    readHigh = false;
}
void PIT8254::Counter::latch(uint64_t now) {
    if (!countLatched) {
        latched = value(now);
        countLatched = true;
        latchHigh = false;
    }
}
void PIT8254::Counter::latchStatus(uint64_t now) {
    if (!statusLatched) {
        status = (output(now) ? 128 : 0) | (nullCount ? 64 : 0) | (rw << 4) | (mode << 1) |
                 (bcd ? 1 : 0);
        statusLatched = true;
    }
}
uint8_t PIT8254::Counter::read(uint64_t now) {
    if (statusLatched) {
        statusLatched = false;
        return status;
    }
    auto v = countLatched ? latched : value(now);
    bool high = rw == 2 || (rw == 3 && (countLatched ? latchHigh : readHigh));
    uint8_t result = high ? v >> 8 : v;
    if (countLatched) {
        if (rw != 3 || latchHigh)
            countLatched = false;
        else
            latchHigh = true;
    } else if (rw == 3)
        readHigh = !readHigh;
    return result;
}
void PIT8254::Counter::write(uint8_t v, uint64_t now) {
    if (rw == 1)
        load(v, now);
    else if (rw == 2)
        load(uint16_t(v) << 8, now);
    else if (!writeHigh) {
        writeValue = v;
        writeHigh = true;
    } else {
        load(writeValue | (uint16_t(v) << 8), now);
        writeHigh = false;
    }
}
void PIT8254::Counter::setGate(bool v, uint64_t now) {
    if (gate == v)
        return;
    if (!v)
        pausedAt = now;
    else if (mode == 1 || mode == 2 || mode == 3 || mode == 5) {
        loadedAt = now;
        lastEdge = 0;
    } else
        loadedAt += now - pausedAt;
    gate = v;
}
bool PIT8254::Counter::edge(uint64_t now) {
    if (nullCount || !gate)
        return false;
    uint64_t elapsed = now - loadedAt;
    if ((mode == 2 || mode == 3) && elapsed < (lastEdge + 1) * reload)
        return false;
    uint64_t edges = (mode == 2 || mode == 3) ? elapsed / reload : elapsed >= reload ? 1 : 0;
    bool raised = edges > lastEdge;
    lastEdge = edges;
    return raised;
}
PIT8254::PIT8254() {
    counters[0].mode = 3;
    counters[1].mode = 2;
    counters[1].reload = 18;
    counters[2].gate = false;
}
void PIT8254::write(unsigned port, uint8_t v, uint64_t now) {
    if (port < 3) {
        counters[port].write(v, now);
        return;
    }
    unsigned channel = v >> 6;
    if (channel == 3) {
        for (unsigned i = 0; i < 3; i++)
            if (v & (2 << i)) {
                if (!(v & 32))
                    counters[i].latch(now);
                if (!(v & 16))
                    counters[i].latchStatus(now);
            }
        return;
    }
    auto &c = counters[channel];
    unsigned rw = (v >> 4) & 3;
    if (!rw) {
        c.latch(now);
        return;
    }
    c.rw = rw;
    c.mode = (v >> 1) & 7;
    if (c.mode > 5)
        c.mode -= 4;
    c.bcd = v & 1;
    c.nullCount = true;
    c.writeHigh = c.readHigh = c.countLatched = c.statusLatched = false;
    c.loadedAt = now;
}
} // namespace pc
