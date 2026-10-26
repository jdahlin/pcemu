#pragma once
#include <cstdint>
#include <deque>
#include <string>
namespace pc {
// PrimeCell UART (PL011), using ARM DDI 0183G, r1p5, as the reference.
// https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5
// The model retains a revision-1 identity; see implementation overview for gaps.
class ArmPl011 {
  public:
    // Initialize the serial console in its disabled reset state.
    ArmPl011();
    // Read/write registers at offsets relative to the platform's UART base.
    uint32_t read_register(unsigned offset);
    void write_register(unsigned offset, uint32_t value);
    // Supply incoming host bytes, or observe the outgoing transcript.
    void receive_host_input(const std::string &text);
    const std::string &transmitted_output() const;
    // Observe unmasked interrupt sources or the combined UARTINTR output.
    uint32_t raw_interrupt_status() const;
    bool uartintr_asserted() const;

  private:
    std::deque<uint8_t> receive;
    std::string transcript;
    uint32_t control, integer_divisor = 0, fractional_divisor = 0, line_control = 0;
    // COMPATIBILITY: UARTIFLS resets to zero here; hardware reset is 0x12 (3.2).
    uint32_t interrupt_mask = 0, fifo_levels = 0;
    // Consume a received byte or transmit a byte through UARTDR.
    uint32_t read_data_register();
    void write_data_register(uint32_t value);
};
} // namespace pc
