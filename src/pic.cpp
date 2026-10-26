// The Intel 8259A programmable interrupt controller (PIC) chooses which device
// interrupt the CPU should service next. Each chip has eight interrupt-request
// inputs. The Interrupt Request Register (IRR) latches requests, the Interrupt
// Mask Register (IMR) disables selected inputs, and the In-Service Register (ISR)
// tracks interrupts already acknowledged by the CPU. The Priority Resolver
// selects a request that outranks active service; priority may rotate.
//
// Initialization Command Words (ICWs) configure vector numbers and cascade mode.
// Operation Command Words (OCWs) change masks, send End Of Interrupt (EOI), rotate
// priority, or select status/poll reads. A poll read itself acknowledges a request.
// The PC/AT board combines two chips; that wiring lives in i8259_pair.cpp.
// IMPLEMENTATION NOTE: Inputs arrive as latched events, not electrical pin levels.
// FIXME: Model ICW1 level-triggered mode, ICW4 special fully nested/buffered modes,
// and MCS-80/85 vector delivery before claiming those datasheet modes are supported.
// Reference: Intel 8259A datasheet, order 231468-003, December 1988.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=3

#include "pic.hpp"
namespace pc {
namespace {
// Initialization and operation command formats, Figures 7 and 8.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=11
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=13
// ICW1 D4: starts the initialization sequence.
constexpr uint8_t InitializationCommand = 0x10;
// ICW1 IC4: request a fourth initialization word.
constexpr uint8_t InitializationWord4Required = 0x01;
// ICW1 SNGL: omit the cascade configuration word.
constexpr uint8_t SingleController = 0x02;
// OCW3 D3: distinguishes status/mask/poll control from OCW2.
constexpr uint8_t OperationCommand3 = 0x08;
// OCW3 RR: update which status register command-port reads return.
constexpr uint8_t ReadRegisterCommand = 0x02;
// OCW3 RIS: read ISR when set, otherwise read IRR.
constexpr uint8_t ReadInServiceRegister = 0x01;
// OCW3 P: the next command-port read acknowledges an interrupt.
constexpr uint8_t PollCommand = 0x04;
// OCW3 ESMM: apply the SMM setting in this write.
constexpr uint8_t EnableSpecialMaskMode = 0x40;
// OCW3 SMM: masked in-service inputs no longer block lower priorities.
constexpr uint8_t SpecialMaskMode = 0x20;
// ICW4 AEOI: acknowledge without leaving an in-service bit set.
constexpr uint8_t AutomaticEndOfInterrupt = 0x02;
// ICW2 T7:T3: the low three vector bits come from the input number.
constexpr uint8_t InterruptVectorBaseMask = 0xf8;
// Poll D7: a valid interrupt level is present in bits 2:0.
constexpr uint8_t PollInterruptPresent = 0x80;
// No initialization word remains; data writes program OCW1/IMR.
constexpr uint8_t InitializationComplete = 0;
// Next data write supplies ICW2, the vector base.
constexpr uint8_t ExpectInitializationWord2 = 2;
// Next data write supplies ICW3, cascade wiring.
constexpr uint8_t ExpectInitializationWord3 = 3;
// Next data write supplies ICW4, processor/EOI mode.
constexpr uint8_t ExpectInitializationWord4 = 4;
// OCW2 bits R:SL:EOI, shifted to bits 2:0. Names describe each operation.
enum class EndOfInterruptCommand : unsigned {
    // Stop automatic priority rotation after future acknowledgements.
    ClearRotateInAutomaticEndOfInterrupt = 0,
    // Clear the highest-priority in-service bit.
    NonSpecificEndOfInterrupt = 1,
    // Encoding with no EOI or rotation effect.
    NoOperation = 2,
    // Clear the in-service bit named by L2:L0.
    SpecificEndOfInterrupt = 3,
    // Rotate priority after future automatic acknowledgements.
    SetRotateInAutomaticEndOfInterrupt = 4,
    // Clear the highest-priority in-service bit and move it to lowest priority.
    RotateOnNonSpecificEndOfInterrupt = 5,
    // Set the L2:L0 input as the lowest-priority input.
    SetPriority = 6,
    // Clear the selected in-service bit and make it lowest priority.
    RotateOnSpecificEndOfInterrupt = 7,
};
// OCW2 stores the R:SL:EOI command in bits 7:5.
constexpr unsigned EndOfInterruptCommandShift = 5;
} // namespace

// Select the first set input in the rotated priority order, or return -1.
// Intel 8259A, Priority Resolver and Fully Nested Mode.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=3
int PIC8259::highest_priority_interrupt(uint8_t bits) const {
    if (!bits)
        return -1;
    unsigned first = (lowest + 1) & InterruptLevelMask;
    unsigned rotated = (unsigned(bits) | (unsigned(bits) << InterruptInputCount)) >> first;
    return (first + __builtin_ctz(rotated)) & InterruptLevelMask;
}
// Resolve an unmasked request that outranks the current in-service interrupt.
// Intel 8259A, Fully Nested Mode and Special Mask Mode.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=15
int PIC8259::pending_interrupt() const {
    int request = highest_priority_interrupt(irr & ~mask);
    if (request < 0)
        return -1;
    int active = highest_priority_interrupt(specialMask ? isr & ~mask : isr);
    return active < 0 || priority_rank(request) < priority_rank(active) ? request : -1;
}
// Consume a request and mark it in service, or apply automatic EOI rotation.
// Intel 8259A, Interrupt Sequence and Automatic End of Interrupt.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=7
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=15
void PIC8259::acknowledge_interrupt(unsigned irq) {
    irr &= ~(1u << irq);
    if (!autoEOI)
        isr |= 1u << irq;
    else if (rotateAuto)
        lowest = irq;
}
// Decode ICW1, OCW2 or OCW3 written to the command port.
// Intel 8259A, Initialization and Operation Command Word formats.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=11
void PIC8259::write_command_register(uint8_t v) {
    if (v & InitializationCommand) {
        init = ExpectInitializationWord2;
        needICW4 = v & InitializationWord4Required;
        single = v & SingleController;
        irr = isr = mask = 0;
        lowest = InterruptLevelMask;
        autoEOI = readISR = pollRead = specialMask = rotateAuto = false;
        return;
    }
    if (v & OperationCommand3) {
        if (v & ReadRegisterCommand)
            readISR = v & ReadInServiceRegister;
        if (v & PollCommand)
            pollRead = true;
        if (v & EnableSpecialMaskMode)
            specialMask = v & SpecialMaskMode;
        return;
    }
    const auto operation = static_cast<EndOfInterruptCommand>(v >> EndOfInterruptCommandShift);
    switch (operation) {
    case EndOfInterruptCommand::ClearRotateInAutomaticEndOfInterrupt:
        rotateAuto = false;
        return;
    case EndOfInterruptCommand::SetRotateInAutomaticEndOfInterrupt:
        rotateAuto = true;
        return;
    case EndOfInterruptCommand::SetPriority:
        lowest = v & InterruptLevelMask;
        return;
    case EndOfInterruptCommand::NoOperation:
        return;
    default:
        break;
    }
    const bool specific = operation == EndOfInterruptCommand::SpecificEndOfInterrupt ||
                          operation == EndOfInterruptCommand::RotateOnSpecificEndOfInterrupt;
    const bool rotate = operation == EndOfInterruptCommand::RotateOnNonSpecificEndOfInterrupt ||
                        operation == EndOfInterruptCommand::RotateOnSpecificEndOfInterrupt;
    int irq = specific ? int(v & InterruptLevelMask) : highest_priority_interrupt(isr);
    if (irq >= 0) {
        isr &= ~(1u << irq);
        if (rotate)
            lowest = irq;
    }
}
// Consume the next initialization word, or set the OCW1 interrupt mask.
// Intel 8259A, Initialization Sequence and OCW1.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=10
void PIC8259::write_data_register(uint8_t v) {
    if (init == ExpectInitializationWord2) {
        base = v & InterruptVectorBaseMask;
        init = single ? (needICW4 ? ExpectInitializationWord4 : InitializationComplete)
                      : ExpectInitializationWord3;
    } else if (init == ExpectInitializationWord3)
        init = needICW4 ? ExpectInitializationWord4 : InitializationComplete;
    else if (init == ExpectInitializationWord4) {
        autoEOI = v & AutomaticEndOfInterrupt;
        init = InitializationComplete;
    } else
        mask = v;
}
// Return selected IRR/ISR status, or perform a poll acknowledgement.
// Intel 8259A, Poll Command and Reading the 8259A Status.
// https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf#page=16
uint8_t PIC8259::read_command_register() {
    if (pollRead) {
        pollRead = false;
        int irq = pending_interrupt();
        if (irq < 0)
            return 0;
        acknowledge_interrupt(irq);
        return PollInterruptPresent | irq;
    }
    return readISR ? isr : irr;
}
} // namespace pc
