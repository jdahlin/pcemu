#pragma once
#include <cstdint>
namespace pc {
// Intel 8259A Programmable Interrupt Controller, 231468-003 (December 1988).
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf
// See pic.cpp for request/mask/service flow and model limitations.
struct PIC8259 {
    // Eight inputs IR0..IR7; three bits encode an input or a rotated priority.
    static constexpr unsigned InterruptInputCount = 8;
    static constexpr unsigned InterruptLevelMask = InterruptInputCount - 1;
    // An IMR bit of one suppresses its corresponding interrupt request.
    static constexpr uint8_t AllInterruptsMasked = 0xff;
    // IMPLEMENTATION NOTE: PC-compatible initial vector block before guest ICWs.
    static constexpr uint8_t DefaultInterruptVectorBase = 8;
    // IMR, IRR and ISR retain their canonical acronyms in snapshot-visible state:
    // masked inputs, latched requests and acknowledged/in-service inputs.
    // base is ICW2's vector block; init tracks the next ICW; lowest anchors rotation.
    uint8_t mask = AllInterruptsMasked, irr = 0, isr = 0, base = DefaultInterruptVectorBase,
            init = 0, lowest = InterruptLevelMask;
    bool single = false, needICW4 = false, autoEOI = false, readISR = false, pollRead = false,
         specialMask = false, rotateAuto = false;
    // Latch an IR input in the Interrupt Request Register; board supplies edge events.
    // https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=3
    void raise_interrupt(unsigned irq) {
        irr |= uint8_t(1u << (irq & InterruptLevelMask));
    }
    // Resolve an unmasked request that outranks the current in-service interrupt.
    int pending_interrupt() const;
    // Consume a request and mark it in service, or apply automatic EOI rotation.
    void acknowledge_interrupt(unsigned irq);
    // Decode ICW1, OCW2 or OCW3 written to the command port.
    void write_command_register(uint8_t value);
    // Consume the next initialization word, or set the OCW1 interrupt mask.
    void write_data_register(uint8_t value);
    // Return selected IRR/ISR status, or perform a poll acknowledgement.
    uint8_t read_command_register();

  private:
    // Rank an input relative to the current lowest-priority input (zero is highest).
    // https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=15
    int priority_rank(unsigned irq) const {
        return (irq - lowest - 1) & InterruptLevelMask;
    }
    // Select the first set input in the rotated priority order, or return -1.
    int highest_priority_interrupt(uint8_t bits) const;
};
} // namespace pc
