#include "arm_pl011.hpp"
#include <stdexcept>
namespace pc {
uint32_t ArmPl011::rawInterrupts() const {
    if (!(control & 1)) return 0;
    return ((control & 0x100) ? 0x20u : 0u) |
           ((control & 0x200) && !receive.empty() ? 0x50u : 0u);
}
uint32_t ArmPl011::read(unsigned offset) {
    switch (offset) {
    case 0:
        if (receive.empty()) return 0;
        { auto byte = receive.front(); receive.pop_front(); return byte; }
    case 0x18: return 0x80 | (receive.empty() ? 0x10 : 0); // TX empty, RX empty
    case 0x24: return integerDivisor;
    case 0x28: return fractionalDivisor;
    case 0x2c: return lineControl;
    case 0x30: return control;
    case 0xfe0: return 0x11;
    case 0xfe4: return 0x10;
    case 0xfe8: return 0x14;
    case 0xfec: return 0;
    case 0xff0: return 0x0d;
    case 0xff4: return 0xf0;
    case 0xff8: return 0x05;
    case 0xffc: return 0xb1;
    case 0x34: return fifoLevels;
    case 0x38: return interruptMask;
    case 0x3c: return rawInterrupts();
    case 0x40: return rawInterrupts() & interruptMask;
    case 4: case 0x20: case 0x44: case 0x48:
        return 0;
    default: throw std::runtime_error("Unsupported PL011 read offset: " + std::to_string(offset));
    }
}
void ArmPl011::write(unsigned offset, uint32_t value) {
    switch (offset) {
    case 0:
        if ((control & 0x101) == 0x101) transcript.push_back(char(value & 0xff));
        return;
    case 0x24: integerDivisor = value & 0xffff; return;
    case 0x28: fractionalDivisor = value & 63; return;
    case 0x2c: lineControl = value & 255; return;
    case 0x30: control = value & 0xffff; return;
    case 0x34: fifoLevels = value & 0x3f; return;
    case 0x38: interruptMask = value & 0x7ff; return;
    // RX/TX levels remain asserted while their FIFO condition holds.
    case 4: case 0x20: case 0x44: case 0x48: return;
    default: throw std::runtime_error("Unsupported PL011 write offset: " + std::to_string(offset));
    }
}
void ArmPl011::input(const std::string &text) {
    if (receive.size() + text.size() > 4096)
        throw std::runtime_error("PL011 host input queue overflow");
    for (unsigned char byte : text) receive.push_back(byte);
}
} // namespace pc
