// The PL011 is a universal asynchronous receiver/transmitter (UART): a serial
// console through which guest software exchanges bytes with the host.
// Its Data Register consumes incoming bytes and submits outgoing bytes. Flags
// describe queue occupancy. Control and line registers configure the interface;
// baud-rate divisors normally determine the speed of bits on the serial wires.
// A hardware FIFO is a first-in, first-out queue buffering bytes between accesses.
// Individual receive/transmit interrupt sources are gated by a mask and combined
// onto UARTINTR. The machine maps the registers and wires that output to its interrupt controller.
// Read-only identification registers let software recognize PrimeCell and PL011.
//
// IMPLEMENTATION NOTE: Host input uses a bounded byte queue; transmitted bytes
// enter an immediate transcript. No physical serial wires or bit timing exist.
// FIFO thresholds, receive timeout timing, modem inputs, errors, infrared and
// direct memory access (DMA) are incomplete; local FIXME comments describe gaps.
// COMPATIBILITY: Preserve the existing revision-1 identity and register behavior.
// Reference: PrimeCell UART (PL011) Technical Reference Manual, ARM DDI 0183G,
// r1p5 (including earlier revision descriptions in 1.2).
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=17

#include "arm_pl011.hpp"
#include <cstddef>
#include <stdexcept>
namespace pc {
namespace {
// Offsets relative to the machine-selected base: 3.2 Summary of registers.
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=49
enum class Register : unsigned {
    // UARTDR: transfers a received or transmitted byte.
    DataRegister = 0x000,
    // UARTRSR/UARTECR: reports receive errors or clears their status.
    ReceiveStatusOrErrorClearRegister = 0x004,
    // UARTFR: reports queue state and modem input signals.
    FlagRegister = 0x018,
    // UARTILPR: selects the infrared low-power pulse timing.
    IrDALowPowerCounterRegister = 0x020,
    // UARTIBRD: integer part of the serial bit-rate divisor.
    IntegerBaudRateRegister = 0x024,
    // UARTFBRD: fractional part of the serial bit-rate divisor.
    FractionalBaudRateRegister = 0x028,
    // UARTLCR_H: selects character format and hardware FIFO enable.
    LineControlRegister = 0x02c,
    // UARTCR: enables the UART, transmitter, receiver and modem controls.
    ControlRegister = 0x030,
    // UARTIFLS: selects receive/transmit queue interrupt thresholds.
    InterruptFIFOLevelSelectRegister = 0x034,
    // UARTIMSC: enables individual interrupt sources.
    InterruptMaskSetOrClearRegister = 0x038,
    // UARTRIS: reports interrupt sources before masking.
    RawInterruptStatusRegister = 0x03c,
    // UARTMIS: reports enabled interrupt sources.
    MaskedInterruptStatusRegister = 0x040,
    // UARTICR: acknowledges individual interrupt sources.
    InterruptClearRegister = 0x044,
    // UARTDMACR: enables direct memory access requests.
    DMAControlRegister = 0x048,
    // Part/designer/revision/configuration, byte 0, returned in bits 7:0.
    PeripheralIdentificationRegister0 = 0xfe0,
    // Part/designer/revision/configuration, byte 1, returned in bits 7:0.
    PeripheralIdentificationRegister1 = 0xfe4,
    // Part/designer/revision/configuration, byte 2, returned in bits 7:0.
    PeripheralIdentificationRegister2 = 0xfe8,
    // Part/designer/revision/configuration, byte 3, returned in bits 7:0.
    PeripheralIdentificationRegister3 = 0xfec,
    // Shared PrimeCell signature, byte 0, returned in bits 7:0.
    PrimeCellIdentificationRegister0 = 0xff0,
    // Shared PrimeCell signature, byte 1, returned in bits 7:0.
    PrimeCellIdentificationRegister1 = 0xff4,
    // Shared PrimeCell signature, byte 2, returned in bits 7:0.
    PrimeCellIdentificationRegister2 = 0xff8,
    // Shared PrimeCell signature, byte 3, returned in bits 7:0.
    PrimeCellIdentificationRegister3 = 0xffc,
};
// Bit fields: 3.3.1, 3.3.3 and 3.3.5-3.3.10; identification: 3.3.15-3.3.16.
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=51
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=69
// UARTCR.UARTEN: enables the serial interface.
constexpr uint32_t UARTEnable = 1u << 0;
// UARTCR.TXE: enables transmission when UARTEN is also set.
constexpr uint32_t TransmitEnable = 1u << 8;
// UARTCR.RXE: enables reception when UARTEN is also set.
constexpr uint32_t ReceiveEnable = 1u << 9;
// UARTFR.RXFE: no received byte is available.
constexpr uint32_t ReceiveFIFOEmpty = 1u << 4;
// UARTFR.TXFE: no byte is waiting for transmission.
constexpr uint32_t TransmitFIFOEmpty = 1u << 7;
// UARTRIS.RXRIS: receive queue interrupt source.
constexpr uint32_t ReceiveInterrupt = 1u << 4;
// UARTRIS.TXRIS: transmit queue interrupt source.
constexpr uint32_t TransmitInterrupt = 1u << 5;
// UARTRIS.RTRIS: receive timeout interrupt source.
constexpr uint32_t ReceiveTimeoutInterrupt = 1u << 6;
// UARTDR.DATA and UARTLCR_H occupy the low eight bits.
constexpr uint32_t ByteMask = 0xff;
// UARTIBRD.BAUD DIVINT is a 16-bit divisor.
constexpr uint32_t IntegerDivisorMask = 0xffff;
// UARTFBRD.BAUD DIVFRAC is a six-bit fraction.
constexpr uint32_t FractionalDivisorMask = 0x3f;
// UARTCR exposes sixteen control bits; upper bits are reserved.
constexpr uint32_t ControlRegisterMask = 0xffff;
// UARTIFLS holds two three-bit threshold selectors.
constexpr uint32_t FIFOLevelSelectMask = 0x3f;
// UARTIMSC has eleven interrupt-enable bits.
constexpr uint32_t InterruptMask = 0x7ff;
// Part 0x011, ARM designer 0x41, revision 1 (r1p1), configuration 0.
constexpr uint32_t PeripheralIdentification = 0x00141011;
// Shared signature recognizing ARM PrimeCell identification; not the part number.
constexpr uint32_t PrimeCellIdentification = 0xb105f00d;
// Implementation queue limit in bytes; not the hardware FIFO depth.
constexpr size_t HostReceiveCapacity = 4096;
} // namespace

// Initialize control state with transmitter/receiver selected but UART disabled.
// ARM DDI 0183G, 3.3.8 Control Register, UARTCR.
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=61
ArmPl011::ArmPl011() : control(TransmitEnable | ReceiveEnable) {}

// Report interrupt sources before applying the interrupt mask.
// ARM DDI 0183G, 2.8 UART interrupts and 3.3.11 Raw Interrupt Status Register.
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=45
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=65
uint32_t ArmPl011::raw_interrupt_status() const {
    if (!(control & UARTEnable))
        return 0;
    // FIXME: Model FIFO trigger crossings and the receive timeout interval rather
    // than asserting receive and timeout sources together for any queued byte.
    // Transmit is always ready because writes drain directly into the transcript.
    return ((control & TransmitEnable) ? TransmitInterrupt : 0u) |
           ((control & ReceiveEnable) && !receive.empty()
                ? ReceiveInterrupt | ReceiveTimeoutInterrupt
                : 0u);
}

// Return whether an enabled source asserts the combined UARTINTR output.
// ARM DDI 0183G, 2.8.6 UARTINTR.
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=46
bool ArmPl011::uartintr_asserted() const {
    return (raw_interrupt_status() & interrupt_mask) != 0;
}

// Read a received data byte and remove it from the receive queue.
// ARM DDI 0183G, 3.3.1 Data Register, UARTDR.
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=51
uint32_t ArmPl011::read_data_register() {
    // IMPLEMENTATION NOTE: Empty reads return zero, without synthesizing an error.
    if (receive.empty())
        return 0;
    auto byte = receive.front();
    receive.pop_front();
    return byte;
}

// Transmit the low data byte when both UARTEN and TXE are enabled.
// ARM DDI 0183G, 3.3.1 Data Register, UARTDR and 3.3.8 Control Register.
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=51
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=61
void ArmPl011::write_data_register(uint32_t value) {
    // IMPLEMENTATION NOTE: Transmission completes immediately into a host transcript.
    if ((control & (UARTEnable | TransmitEnable)) == (UARTEnable | TransmitEnable))
        transcript.push_back(char(value & ByteMask));
}

// Read the addressed register, including queue flags and identification bytes.
// ARM DDI 0183G, 3.2 Summary of registers and 3.3 Register descriptions.
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=49
uint32_t ArmPl011::read_register(unsigned offset) {
    switch (static_cast<Register>(offset)) {
    case Register::DataRegister:
        return read_data_register();
    case Register::FlagRegister:
        // 3.3.3 Flag Register: TXFE is always set; RXFE follows the host queue.
        // https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=54
        // FIXME: Model modem input flags, BUSY and finite hardware FIFO fullness.
        return TransmitFIFOEmpty | (receive.empty() ? ReceiveFIFOEmpty : 0);
    case Register::IntegerBaudRateRegister:
        return integer_divisor;
    case Register::FractionalBaudRateRegister:
        return fractional_divisor;
    case Register::LineControlRegister:
        return line_control;
    case Register::ControlRegister:
        return control;
    // 3.3.15 Peripheral identification registers, UARTPeriphID0-3.
    // https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=69
    // Software combines the bytes to identify part 0x011 and ARM designer 0x41.
    case Register::PeripheralIdentificationRegister0:
        return uint8_t(PeripheralIdentification);
    case Register::PeripheralIdentificationRegister1:
        return uint8_t(PeripheralIdentification >> 8);
    case Register::PeripheralIdentificationRegister2:
        return uint8_t(PeripheralIdentification >> 16);
    case Register::PeripheralIdentificationRegister3:
        return uint8_t(PeripheralIdentification >> 24);
    // 3.3.16 PrimeCell identification registers, UARTPCellID0-3.
    // https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=71
    // This shared signature identifies the PrimeCell identification scheme, not
    // the specific UART. Each register returns one byte in bits 7:0; software
    // reconstructs ID0 | (ID1 << 8) | (ID2 << 16) | (ID3 << 24).
    case Register::PrimeCellIdentificationRegister0:
        return uint8_t(PrimeCellIdentification);
    case Register::PrimeCellIdentificationRegister1:
        return uint8_t(PrimeCellIdentification >> 8);
    case Register::PrimeCellIdentificationRegister2:
        return uint8_t(PrimeCellIdentification >> 16);
    case Register::PrimeCellIdentificationRegister3:
        return uint8_t(PrimeCellIdentification >> 24);
    // 3.3.9-3.3.12: threshold selection and raw/masked interrupt status.
    // https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=63
    case Register::InterruptFIFOLevelSelectRegister:
        return fifo_levels;
    case Register::InterruptMaskSetOrClearRegister:
        return interrupt_mask;
    case Register::RawInterruptStatusRegister:
        return raw_interrupt_status();
    case Register::MaskedInterruptStatusRegister:
        return raw_interrupt_status() & interrupt_mask;
    case Register::ReceiveStatusOrErrorClearRegister:
    case Register::IrDALowPowerCounterRegister:
    case Register::InterruptClearRegister:
    case Register::DMAControlRegister:
        // COMPATIBILITY: Unsupported status/control and write-only reads return zero.
        return 0;
    default:
        throw std::runtime_error("Unsupported PL011 read offset: " + std::to_string(offset));
    }
}

// Write data or configuration while retaining the defined register bit widths.
// ARM DDI 0183G, 3.3.5-3.3.14 (divisors, line/control and interrupts).
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=55
void ArmPl011::write_register(unsigned offset, uint32_t value) {
    switch (static_cast<Register>(offset)) {
    case Register::DataRegister:
        write_data_register(value);
        return;
    // FIXME: Apply baud/line/FIFO controls to transfer timing and queue behavior.
    // The current model stores them only for register readback.
    case Register::IntegerBaudRateRegister:
        integer_divisor = value & IntegerDivisorMask;
        return;
    case Register::FractionalBaudRateRegister:
        fractional_divisor = value & FractionalDivisorMask;
        return;
    case Register::LineControlRegister:
        line_control = value & ByteMask;
        return;
    case Register::ControlRegister:
        control = value & ControlRegisterMask;
        return;
    // FIXME: Use the documented UARTIFLS reset value (0x12); retain zero until
    // compatibility is validated (3.2 and 3.3.9).
    // https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=63
    case Register::InterruptFIFOLevelSelectRegister:
        fifo_levels = value & FIFOLevelSelectMask;
        return;
    case Register::InterruptMaskSetOrClearRegister:
        interrupt_mask = value & InterruptMask;
        return;
    case Register::ReceiveStatusOrErrorClearRegister:
    case Register::IrDALowPowerCounterRegister:
    case Register::DMAControlRegister:
        // FIXME: Implement receive errors (3.3.2), infrared (3.3.4) and DMA (3.3.14).
        // https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=52
        // https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=68
        return;
    case Register::InterruptClearRegister:
        // FIXME: Honor write-one clearing and subsequent threshold/timeout events.
        // COMPATIBILITY: Queue-level conditions remain asserted until data drains.
        // 3.3.13 Interrupt Clear Register, UARTICR.
        // https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=67
        return;
    default:
        throw std::runtime_error("Unsupported PL011 write offset: " + std::to_string(offset));
    }
}

// Queue host-provided bytes for subsequent guest Data Register reads.
// ARM DDI 0183G, 3.3.1 Data Register, UARTDR (guest-facing receive path).
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=51
void ArmPl011::receive_host_input(const std::string &text) {
    // IMPLEMENTATION NOTE: Bound host buffering independently of hardware FIFOs.
    // COMPATIBILITY: Host input queues even when the UART/receiver is disabled.
    if (receive.size() + text.size() > HostReceiveCapacity)
        throw std::runtime_error("PL011 host input queue overflow");
    for (unsigned char byte : text)
        receive.push_back(byte);
}

// Expose the accumulated transmitted bytes to the host console.
// ARM DDI 0183G, 3.3.1 Data Register, UARTDR (guest-facing transmit path).
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5#page=51
// IMPLEMENTATION NOTE: The transcript is a host observation buffer, not a UART register.
const std::string &ArmPl011::transmitted_output() const {
    return transcript;
}
} // namespace pc
