#include "i8259_pair.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
#include <tuple>
using namespace pc;
using State = I8259PairDevice::State;
static unsigned checks;
static void check(bool value, const char *message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
// Frozen pre-optimization polling algorithm, including mirror side effects.
static std::optional<uint8_t> reference(State &s, bool ready) {
    auto mirrors = [&] {
        s.timerPending = s.chips[0].irr & 1;
        s.timerInService = s.chips[0].isr & 1;
        s.keyboardInService = s.chips[0].isr & 2;
        s.ataInService = s.chips[1].isr & 0x40;
    };
    auto &master = s.chips[0]; auto &slave = s.chips[1];
    int pendingSlave = (slave.irr & ~slave.mask) ? slave.pending_interrupt() : -1;
    if (pendingSlave >= 0 && !s.cascadeLine) {
        master.raise_interrupt(2);
        s.cascadeLine = true;
    }
    if (pendingSlave < 0) s.cascadeLine = false;
    mirrors();
    if (!ready || !(master.irr & ~master.mask)) return std::nullopt;
    int irq = master.pending_interrupt();
    if (irq < 0) return std::nullopt;
    master.acknowledge_interrupt(irq);
    uint8_t vector = master.base + irq;
    if (irq == 2) {
        if (pendingSlave >= 0) {
            slave.acknowledge_interrupt(pendingSlave);
            vector = slave.base + pendingSlave;
        } else vector = slave.base + 7;
        s.cascadeLine = false;
    }
    mirrors();
    return vector;
}
static auto fields(const PIC8259 &p) {
    return std::tie(p.mask,p.irr,p.isr,p.base,p.init,p.lowest,p.single,p.needICW4,
                    p.autoEOI,p.readISR,p.pollRead,p.specialMask,p.rotateAuto);
}
static void compare(State initial, bool ready) {
    I8259PairDevice device;
    // Set otherwise unreachable stale compatibility/cascade states as well.
    const_cast<State &>(device.state()) = initial;
    auto expected = reference(initial, ready);
    check(device.next_interrupt_vector(ready) == expected, "PIC vector/readiness differs from reference");
    const auto &actual = device.state();
    for (unsigned n = 0; n < 2; ++n) {
        check(fields(actual.chips[n]) == fields(initial.chips[n]), "PIC chip state differs");
        check(actual.picBase[n] == initial.picBase[n] && actual.picInit[n] == initial.picInit[n],
              "PIC compatibility configuration changed");
    }
    check(actual.cascadeLine == initial.cascadeLine && actual.timerPending == initial.timerPending &&
          actual.timerInService == initial.timerInService && actual.keyboardInService == initial.keyboardInService &&
          actual.ataInService == initial.ataInService, "PIC cascade/mirrors differ");
}
int main() {
    try {
        for (unsigned master = 0; master < 256; ++master)
            for (unsigned slave = 0; slave < 256; ++slave)
                for (bool ready : {false,true})
                    for (bool cascade : {false,true}) {
                        State s;
                        s.chips[0].isr = master; s.chips[1].isr = slave;
                        s.chips[0].mask = slave; s.chips[1].mask = master;
                        s.cascadeLine = cascade;
                        s.timerPending = s.timerInService = s.keyboardInService = s.ataInService = true;
                        compare(s, ready);
                    }
        std::mt19937 random(0x8259);
        for (unsigned n = 0; n < 100000; ++n) {
            State s;
            for (auto &p : s.chips) {
                p.mask=random(); p.irr=random(); p.isr=random(); p.base=random() & 0xf8;
                p.init=random() % 5; p.lowest=random() & 7;
                p.single=random() & 1; p.needICW4=random() & 1; p.autoEOI=random() & 1;
                p.readISR=random() & 1; p.pollRead=random() & 1;
                p.specialMask=random() & 1; p.rotateAuto=random() & 1;
            }
            s.cascadeLine=random() & 1; s.timerPending=random() & 1;
            s.timerInService=random() & 1; s.keyboardInService=random() & 1; s.ataInService=random() & 1;
            compare(s, random() & 1);
        }
        std::cout << "Passed " << checks << " PIC polling checks\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
