#include "arm_devices.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
namespace pc {
ArmPl031::ArmPl031(std::function<int64_t()> clock) : seconds(std::move(clock)) {
    if (!seconds) seconds = [] {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
    previous = seconds();
}
int64_t ArmPl031::update() {
    const int64_t current = seconds() + adjustment;
    // Latch an alarm even if the host does not poll on the exact matching second.
    // A backwards host clock step must not look like a 32-bit counter wrap.
    if (current > previous) {
        const uint64_t elapsed = uint64_t(current - previous);
        const uint32_t distance = match - uint32_t(previous);
        if (elapsed >= (1ull << 32) || (distance && distance <= elapsed)) pending = true;
    }
    previous = current;
    return current;
}
bool ArmPl031::irq() {
    // Raw status is sampled on reads/unmask. A masked alarm needs no host poll.
    if (!masked) return false;
    update();
    return pending;
}
uint32_t ArmPl031::read(unsigned offset) {
    auto current = update();
    switch (offset) {
    case 0x00: return uint32_t(current);
    case 0x04: return match;
    case 0x08: return loaded;
    case 0x0c: return 1; // Enabled at reset; enable is sticky on PL031.
    case 0x10: return masked;
    case 0x14: return pending;
    case 0x18: return masked && pending;
    case 0xfe0: return 0x31;
    case 0xfe4: return 0x10;
    case 0xfe8: return 0x04;
    case 0xfec: return 0x00;
    case 0xff0: return 0x0d;
    case 0xff4: return 0xf0;
    case 0xff8: return 0x05;
    case 0xffc: return 0xb1;
    default: return 0;
    }
}
void ArmPl031::write(unsigned offset, uint32_t value) {
    update();
    switch (offset) {
    case 0x04: match = value; if (match == uint32_t(previous)) pending = true; break;
    case 0x08:
        loaded = value;
        adjustment = int64_t(value) - seconds();
        previous = value;
        if (match == value) pending = true;
        break;
    case 0x10: masked = value & 1; break;
    case 0x1c: if (value & 1) pending = false; break;
    default: break;
    }
}
bool ArmOsLock::access(uint64_t syndrome, uint64_t &value) {
    // ESR_EL2.ISS: ignore Rt, retain direction and every register encoding bit.
    if ((syndrome >> 26 & 63) != 0x18 || !(syndrome & (1ull << 25))) return false;
    const auto encoding = syndrome & 0x3ffc1f;
    if (encoding == 0x280407) { // MRS OSDLR_EL1: S2_0_C1_C3_4.
        value = doubleLocked;
        return true;
    }
    if (encoding == 0x280406) { // MSR OSDLR_EL1; double lock is sticky until reset.
        doubleLocked |= value & 1;
        return true;
    }
    if (encoding == 0x280403) { // MRS OSLSR_EL1: S2_0_C1_C1_4.
        value = 8 | (locked ? 2 : 0); // OSLM=0b10, nTT=0, OSLK.
        return true;
    }
    if (encoding == 0x280400) { // MSR OSLAR_EL1: S2_0_C1_C0_4.
        if (!doubleLocked) locked = value & 1;
        return true;
    }
    return false;
}

ArmPsciResult armPsciCall(uint32_t function, uint64_t arg0, uint64_t arg1) {
    if (!(function & 0x40000000)) { arg0 = uint32_t(arg0); arg1 = uint32_t(arg1); }
    switch (function) {
    case 0x84000000: return {2}; // PSCI 0.2
    case 0x84000001: case 0xc4000001:
        // Standby can wake spuriously; deeper states have not been implemented.
        return {arg0 == 0 ? 0 : -2};
    case 0x84000002: case 0x84000008: return {0, ArmPowerAction::Off};
    case 0x84000009: return {0, ArmPowerAction::Reset};
    case 0x84000003: case 0xc4000003: return {arg0 == 0 ? -4 : -2};
    case 0x84000004: case 0xc4000004: return {arg0 == 0 && arg1 == 0 ? 0 : -2};
    case 0x84000006: return {2}; // No resident trusted OS.
    default: return {-1};
    }
}
uint64_t armMmioLoad(uint64_t value, unsigned width, bool signExtend, bool wideRegister) {
    if (width != 1 && width != 2 && width != 4 && width != 8)
        throw std::runtime_error("Invalid ARM MMIO load width");
    if (width < 8) {
        value &= (1ull << (width * 8)) - 1;
        if (signExtend) {
            const auto sign = 1ull << (width * 8 - 1);
            value = (value ^ sign) - sign;
        }
    }
    return wideRegister ? value : uint32_t(value);
}

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
