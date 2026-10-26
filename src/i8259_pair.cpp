// PC/AT interrupt routing uses a primary (master) and secondary (slave) 8259A.
// The secondary's output is wired to primary IR2. Requests on secondary IR0..7
// therefore reach the CPU through primary IR2, giving sixteen board IRQ numbers
// with IRQ2 occupied by the cascade. Each chip independently masks and resolves
// priority. A cascade acknowledgement consumes primary IR2 and one secondary IRQ.
// The returned vector selects the guest's interrupt handler.
//
// IMPLEMENTATION NOTE: The board supplies CPU readiness; this wrapper does not
// read CPU flags. Legacy diagnostic/snapshot mirrors track device state but do
// not select interrupts. Fixed PC/AT cascade wiring overrides ICW3 programming.
// Reference: Intel 8259A, 231468-003, December 1988, Cascade Mode.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
// Board composition: docs/pc-at-device-model.md, Compatibility inventory.

#include "i8259_pair.hpp"
namespace pc {
// Read IMR on the data port or the chip-selected IRR/ISR/poll result on the command port.
// Intel 8259A, Reading the 8259A Status.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=17
uint8_t I8259PairDevice::read_register(unsigned chip, bool data) {
    return data ? state_.chips[chip].mask : state_.chips[chip].read_command_register();
}
// Route an ICW/OCW write to its chip and refresh the compatibility mirrors.
// Intel 8259A, Programming the 8259A.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=9
void I8259PairDevice::write_register(unsigned chip, bool data, uint8_t value) {
    if (data)
        state_.chips[chip].write_data_register(value);
    else
        state_.chips[chip].write_command_register(value);
    for (unsigned i = 0; i < ControllerCount; ++i)
        state_.picBase[i] = state_.chips[i].base;
    update_compatibility_mirrors();
}
// Resolve the cascade and acknowledge the next deliverable CPU vector.
// Intel 8259A, Cascade Mode and Interrupt Sequence.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=19
std::optional<uint8_t> I8259PairDevice::acknowledge_pending_vector(bool cpuReady) {
    // COMPATIBILITY: Secondary requests are edge-latched onto the fixed IR2 wire.
    auto &master = state_.chips[PrimaryController];
    auto &slave = state_.chips[SecondaryController];
    int pendingSlave = (slave.irr & ~slave.mask) ? slave.pending_interrupt() : -1;
    if (pendingSlave >= 0 && !state_.cascadeLine) {
        master.raise_interrupt(CascadeInterrupt);
        state_.cascadeLine = true;
    }
    if (pendingSlave < 0)
        state_.cascadeLine = false;
    update_compatibility_mirrors();
    if (!cpuReady || !(master.irr & ~master.mask))
        return std::nullopt;
    int irq = master.pending_interrupt();
    if (irq < 0)
        return std::nullopt;
    master.acknowledge_interrupt(irq);
    uint8_t vector = master.base + irq;
    if (irq == CascadeInterrupt) {
        if (pendingSlave >= 0) {
            slave.acknowledge_interrupt(pendingSlave);
            vector = slave.base + pendingSlave;
        } else
            vector = slave.base + SpuriousInterruptLevel;
        state_.cascadeLine = false;
    }
    update_compatibility_mirrors();
    return vector;
}
} // namespace pc
