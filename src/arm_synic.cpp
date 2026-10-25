#include "arm_synic.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
namespace {
constexpr uint16_t invalidParameter = 5, invalidVp = 14, insufficientBuffers = 0x13,
                   invalidState = 0x18, operationFailed = 0x71;
constexpr uint64_t masked = 1ull << 16, polling = 1ull << 18;
void put(std::span<uint8_t> bytes, size_t at, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}
}
ArmSynic::ArmSynic(std::span<uint8_t> memory, uint64_t start, unsigned count)
    : ram(memory), base(start), cpus(count) {
    if (!count || count > state.size()) throw std::runtime_error("Invalid SynIC VP count");
}
std::span<uint8_t> ArmSynic::page(uint64_t reg) const {
    auto address = reg & ~uint64_t(4095);
    if (!(reg & 1) || address < base || address - base > ram.size() ||
        4096 > ram.size() - (address - base)) return {};
    return ram.subspan(size_t(address - base), 4096);
}
bool ArmSynic::needsService() const {
    for (unsigned vp = 0; vp < cpus; ++vp) {
        const auto &v = state[vp];
        if (!(v.control & 1) || page(v.messages).empty()) continue;
        for (const auto &queue : v.queue) if (!queue.empty()) return true;
    }
    return false;
}
uint16_t ArmSynic::service() {
    uint16_t firstError = 0;
    for (unsigned vp = 0; vp < cpus; ++vp) for (unsigned sint = 0; sint < 16; ++sint) {
        auto status = drain(vp, sint);
        if (status && !firstError) firstError = status;
    }
    return firstError;
}
uint16_t ArmSynic::read(uint32_t name, unsigned vp, uint64_t &value) const {
    value = 0;
    if (vp >= cpus) return invalidVp;
    const auto &v = state[vp];
    if (name < 0xa0010 && name >= 0xa0000) value = v.sint[name - 0xa0000];
    else switch (name) {
    case 0xa0010: value = v.control; break;
    case 0xa0011: value = 1; break;
    case 0xa0012: value = v.events; break;
    case 0xa0013: value = v.messages; break;
    case 0xa0014: break;
    default: return invalidParameter;
    }
    return 0;
}
uint16_t ArmSynic::write(uint32_t name, unsigned vp, uint64_t value) {
    if (vp >= cpus) return invalidVp;
    auto &v = state[vp];
    if (name >= 0xa0000 && name < 0xa0010) {
        // ARM uses a private INTID (SGI or PPI). AutoEOI/proxy are not implemented; never
        // silently accept the x64 APIC behavior on the virtual GIC.
        if (value & ~(uint64_t(31) | masked | polling)) return invalidParameter;
        v.sint[name - 0xa0000] = value;
        return 0;
    }
    switch (name) {
    case 0xa0010:
        if (value & ~uint64_t(1)) return invalidParameter;
        v.control = value; return 0;
    case 0xa0012: case 0xa0013: {
        if ((value & 0xffe) || ((value & 1) && page(value).empty())) return invalidParameter;
        auto &reg = name == 0xa0012 ? v.events : v.messages;
        // Clear a newly installed overlay. Rewriting the same register must
        // not destroy notifications already deposited in guest memory.
        if ((value & 1) && (!reg || (reg & ~uint64_t(4095)) != (value & ~uint64_t(4095))))
            std::fill(page(value).begin(), page(value).end(), uint8_t(0));
        reg = value; return 0;
    }
    case 0xa0014:
        if (value) return invalidParameter;
        for (unsigned sint = 0; sint < 16; ++sint) {
            auto status = drain(vp, sint);
            if (status) return status;
        }
        return 0;
    default: return invalidParameter;
    }
}
bool ArmSynic::notify(unsigned vp, unsigned sint) {
    auto config = state[vp].sint[sint];
    if ((config & polling) || (config & masked)) return true;
    return interrupt && interrupt(1ull << vp, unsigned(config & 31));
}
uint16_t ArmSynic::signalEvent(unsigned vp, unsigned sint, unsigned flag, bool &newlySignaled) {
    newlySignaled = false;
    if (vp >= cpus) return invalidVp;
    if (sint >= 16 || flag >= 2048) return invalidParameter;
    auto &v = state[vp];
    auto memory = page(v.events);
    if (!(v.control & 1) || memory.empty() ||
        ((v.sint[sint] & masked) && !(v.sint[sint] & polling))) return invalidState;
    auto &byte = memory[sint * 256 + flag / 8];
    auto bit = uint8_t(1u << (flag % 8));
    if (byte & bit) return 0;
    byte |= bit;
    if (!notify(vp, sint)) { byte &= uint8_t(~bit); return operationFailed; }
    newlySignaled = true;
    return 0;
}
uint16_t ArmSynic::drain(unsigned vp, unsigned sint) {
    auto &v = state[vp];
    auto &queue = v.queue[sint];
    auto memory = page(v.messages);
    if (queue.empty() || !(v.control & 1) || memory.empty()) return 0;
    auto slot = memory.subspan(sint * 256, 256);
    if (slot[0] || slot[1] || slot[2] || slot[3]) {
        slot[5] |= 1; // More messages await this occupied slot.
        return 0;
    }
    Message previous;
    std::copy(slot.begin(), slot.end(), previous.begin());
    std::copy(queue.front().begin(), queue.front().end(), slot.begin());
    slot[5] = queue.size() > 1 ? 1 : 0;
    if (!notify(vp, sint)) {
        std::copy(previous.begin(), previous.end(), slot.begin());
        return operationFailed;
    }
    queue.pop_front();
    return 0;
}
uint16_t ArmSynic::tryPostMessage(unsigned vp, unsigned sint, uint32_t type,
                                 uint64_t sender, std::span<const uint8_t> payload) {
    if (vp >= cpus) return invalidVp;
    if (sint >= 16 || !type || payload.size() > 240) return invalidParameter;
    auto &v = state[vp];
    auto memory = page(v.messages);
    if (!(v.control & 1) || memory.empty()) return invalidState;
    auto slot = memory.subspan(sint * 256, 256);
    if (!v.queue[sint].empty() || slot[0] || slot[1] || slot[2] || slot[3]) {
        slot[5] |= 1;
        return 0x14; // NotAcknowledged: caller retains its dedicated buffer.
    }
    return postMessage(vp, sint, type, sender, payload);
}
uint16_t ArmSynic::postMessage(unsigned vp, unsigned sint, uint32_t type,
                              uint64_t sender, std::span<const uint8_t> payload) {
    if (vp >= cpus) return invalidVp;
    if (sint >= 16 || !type || payload.size() > 240) return invalidParameter;
    auto &v = state[vp];
    if (!(v.control & 1) || page(v.messages).empty()) return invalidState;
    auto &queue = v.queue[sint];
    if (queue.size() >= 16) return insufficientBuffers;
    Message message{};
    put(message, 0, type, 4); message[4] = uint8_t(payload.size());
    put(message, 8, sender, 8);
    std::copy(payload.begin(), payload.end(), message.begin() + 16);
    queue.push_back(message);
    auto status = drain(vp, sint);
    if (status) queue.pop_back(); // Failed posting is safe for a caller to retry.
    return status;
}
}
