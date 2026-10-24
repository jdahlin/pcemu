#include "arm_devices.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
ArmNorFlash::ArmNorFlash(std::span<const uint8_t> initial)
    : data(initial.begin(), initial.end()), visible(data) {
    if (data.empty() || data.size() % 0x40000)
        throw std::runtime_error("NOR size must be a multiple of 256 KiB");
}
void ArmNorFlash::dirty(size_t offset, size_t length) {
    dirtyStart = std::min(dirtyStart, offset);
    dirtyEnd = std::max(dirtyEnd, offset + length);
}
void ArmNorFlash::array() {
    if (statusAddress != SIZE_MAX) {
        std::copy_n(data.begin() + statusAddress, statusLength, visible.begin() + statusAddress);
        dirty(statusAddress, statusLength);
        statusAddress = SIZE_MAX;
    }
}
void ArmNorFlash::status(size_t offset) {
    array();
    statusAddress = offset;
    statusLength = 4;
    visible[offset] = visible[offset + 2] = 0x80;
    visible[offset + 1] = visible[offset + 3] = 0;
    dirty(offset, 4);
}
void ArmNorFlash::program(size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        visible[offset + i] = data[offset + i] &= uint8_t(value >> (i * 8));
    dirty(offset, 4);
}
void ArmNorFlash::write(uint64_t offset64, uint32_t value) {
    if (offset64 % 4 || offset64 > data.size() - 4)
        throw std::runtime_error("NOR access outside array or unaligned");
    size_t offset = size_t(offset64);
    unsigned command = value & 0xffff;
    switch (pending) {
    case Pending::Word:
        array(); program(offset, value); pending = Pending::None; status(blockAddress); return;
    case Pending::Erase:
        if (command != 0xd0) throw std::runtime_error("Invalid NOR erase confirmation");
        array();
        std::fill_n(data.begin() + blockAddress, 0x40000, 0xff);
        std::fill_n(visible.begin() + blockAddress, 0x40000, 0xff);
        dirty(blockAddress, 0x40000);
        pending = Pending::None; status(blockAddress); return;
    case Pending::Lock:
        if (command != 0xd0 && command != 1) throw std::runtime_error("Invalid NOR lock confirmation");
        pending = Pending::None; status(blockAddress); return;
    case Pending::Count:
        remaining = (value & 0xffff) + 1;
        if (remaining > 32) throw std::runtime_error("NOR write buffer overflow");
        buffer.clear(); pending = Pending::Buffer; return;
    case Pending::Buffer:
        if (offset / 0x40000 != blockAddress / 0x40000)
            throw std::runtime_error("NOR buffer crosses erase block");
        buffer.push_back({offset, value});
        if (--remaining == 0) pending = Pending::Confirm;
        return;
    case Pending::Confirm:
        if (command != 0xd0) throw std::runtime_error("Invalid NOR buffer confirmation");
        array();
        for (auto [address, word] : buffer) program(address, word);
        pending = Pending::None; status(blockAddress); return;
    case Pending::None: break;
    }
    switch (command) {
    case 0xff: array(); return;
    case 0x90:
        array();
        if (offset + 12 > data.size()) throw std::runtime_error("NOR ID beyond bank");
        statusAddress = offset; statusLength = 12;
        for (unsigned i = 0; i < 12; ++i) visible[offset + i] = 0;
        visible[offset] = visible[offset + 2] = 0x89;
        visible[offset + 4] = visible[offset + 6] = 0x18;
        dirty(offset, 12); return;
    case 0x70: status(offset); return;
    case 0x50: status(offset); return;
    case 0x40: case 0x10: array(); blockAddress = offset; pending = Pending::Word; return;
    case 0x20: array(); blockAddress = offset & ~size_t(0x3ffff); pending = Pending::Erase; return;
    case 0x60: array(); blockAddress = offset; pending = Pending::Lock; return;
    case 0xe8: blockAddress = offset; status(offset); pending = Pending::Count; return;
    default: throw std::runtime_error("Unsupported NOR command: " + std::to_string(command));
    }
}
std::pair<size_t, size_t> ArmNorFlash::takeDirty() {
    auto result = std::pair{dirtyStart, dirtyEnd};
    dirtyStart = SIZE_MAX; dirtyEnd = 0;
    return result;
}
} // namespace pc

namespace pc {
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
    // RSR/ECR, interrupt and DMA registers are inert in this polled subset.
    case 4: case 0x20: case 0x34: case 0x38: case 0x3c: case 0x40: case 0x44: case 0x48:
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
    case 4: case 0x20: case 0x34: case 0x38: case 0x44: case 0x48: return;
    default: throw std::runtime_error("Unsupported PL011 write offset: " + std::to_string(offset));
    }
}
void ArmPl011::input(const std::string &text) {
    if (receive.size() + text.size() > 4096)
        throw std::runtime_error("PL011 host input queue overflow");
    for (unsigned char byte : text) receive.push_back(byte);
}
} // namespace pc
