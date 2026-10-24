#include "i8259_pair.hpp"
namespace pc {
void I8259PairDevice::updateMirrors() {
    state_.timerPending = state_.chips[0].irr & 1;
    state_.timerInService = state_.chips[0].isr & 1;
    state_.keyboardInService = state_.chips[0].isr & 2;
    state_.ataInService = state_.chips[1].isr & 0x40;
}
uint8_t I8259PairDevice::read(unsigned chip, bool data) {
    return data ? state_.chips[chip].mask : state_.chips[chip].read();
}
void I8259PairDevice::write(unsigned chip, bool data, uint8_t value) {
    if (data) state_.chips[chip].data(value); else state_.chips[chip].command(value);
    for (unsigned i = 0; i < 2; ++i) state_.picBase[i] = state_.chips[i].base;
    updateMirrors();
}
std::optional<uint8_t> I8259PairDevice::nextVector(bool cpuReady) {
    auto &master = state_.chips[0]; auto &slave = state_.chips[1];
    int pendingSlave = (slave.irr & ~slave.mask) ? slave.pending() : -1;
    if (pendingSlave >= 0 && !state_.cascadeLine) {
        master.raise(2);
        state_.cascadeLine = true;
    }
    if (pendingSlave < 0) state_.cascadeLine = false;
    updateMirrors();
    if (!cpuReady || !(master.irr & ~master.mask)) return std::nullopt;
    int irq = master.pending();
    if (irq < 0) return std::nullopt;
    master.acknowledge(irq);
    uint8_t vector = master.base + irq;
    if (irq == 2) {
        if (pendingSlave >= 0) {
            slave.acknowledge(pendingSlave);
            vector = slave.base + pendingSlave;
        } else vector = slave.base + 7;
        state_.cascadeLine = false;
    }
    updateMirrors();
    return vector;
}
} // namespace pc
