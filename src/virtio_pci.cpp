#include "virtio_pci.hpp"
#include <stdexcept>
#include <algorithm>
namespace pc {
namespace {
void put(std::array<uint8_t, 256> &bytes, unsigned offset, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}
uint64_t mask(unsigned width) { return width == 8 ? UINT64_MAX : (1ull << (8 * width)) - 1; }
void access(unsigned offset, unsigned width, unsigned size) {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width ||
        offset > size || width > size - offset)
        throw std::runtime_error("Invalid virtio PCI access");
}
}
VirtioPciDevice::VirtioPciDevice(VirtioBlock &b, std::function<void(bool)> irq, Message msi)
    : VirtioPciDevice([&b](unsigned o, unsigned w) { return b.read(o, w); },
                     [&b](unsigned o, uint64_t v, unsigned w) { b.write(o, v, w); },
                     2, 0x01000001, 1, std::move(irq), std::move(msi)) {}
VirtioPciDevice::VirtioPciDevice(Reader read, Writer write, uint16_t deviceId,
                               uint32_t classCode, uint16_t queues, std::function<void(bool)> irq, Message msi)
    : reader(std::move(read)), writer(std::move(write)), queueCount(queues), interrupt(std::move(irq)), message(std::move(msi)),
      queueVectors(queues, 0xffff), vectorTable(message ? queues + 1 : 0) {
    if (message && queues > 255) throw std::runtime_error("Too many MSI-X vectors");
    for (auto &entry : vectorTable) entry[12] = 1;
    writeReg(0x38, readReg(0x34));
    put(config, 0, (uint32_t(0x1040 + deviceId) << 16) | 0x1af4, 4);
    put(config, 6, 0x10, 2); // capabilities list
    put(config, 8, classCode, 4); // mass storage/SCSI, revision 1
    put(config, 0x2c, (uint32_t(0x40 + deviceId) << 16) | 0x1af4, 4);
    config[0x34] = 0x40;
    config[0x3c] = 0xff; config[0x3d] = 1;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned o = 0x40 + i * 0x14;
        config[o] = 9; // vendor-specific capability
        config[o + 1] = i == 3 ? (message ? 0x90 : 0) : o + 0x14;
        config[o + 2] = i == 1 ? 20 : 16;
        config[o + 3] = i + 1; // common, notify, ISR, device
        config[o + 4] = 0; // BAR0
        put(config, o + 8, i * 0x1000, 4);
        put(config, o + 12, i == 0 ? 56 : i == 1 ? 0x1000 : i == 2 ? 1 : 64, 4);
        if (i == 1) put(config, o + 16, 4, 4); // notify_off_multiplier
    }
    if (message) {
        config[0x90] = 0x11; // PCI MSI-X capability
        put(config, 0x94, 0x4000, 4); // table in BAR0
        put(config, 0x98, 0x5000, 4); // pending bit array in BAR0
    }
}
uint64_t VirtioPciDevice::configRead(uint64_t offset, unsigned width) const {
    access(offset & 0xfff, width, 4096);
    if (offset >= 256) return mask(width); // only function 00:00.0, standard config space
    auto bytes = config;
    put(bytes, 4, command, 2);
    put(bytes, 6, 0x10 | (irqLevel ? 8 : 0), 2);
    put(bytes, 0x10, barProbe ? uint32_t(~(barSize() - 1)) : bar, 4);
    if (message) put(bytes, 0x92, queueCount | msixControl, 2);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}
void VirtioPciDevice::configWrite(uint64_t offset, uint64_t value, unsigned width) {
    access(offset & 0xfff, width, 4096);
    if (offset >= 256) return;
    if (offset == 0x10 && width == 4) {
        barProbe = uint32_t(value) == 0xffffffff;
        if (!barProbe) bar = uint32_t(value) & uint32_t(~(barSize() - 1));
        return;
    }
    for (unsigned i = 0; i < width; ++i) {
        auto at = offset + i;
        uint8_t byte = value >> (i * 8);
        if (at == 4) command = (command & 0xff00) | (byte & 7);
        if (at == 5) command = (command & 0xff) | (uint16_t(byte & 4) << 8);
        if (at == 0xc || at == 0xd || at == 0x3c) config[at] = byte;
        if (message && at == 0x93) msixControl = uint16_t(byte & 0xc0) << 8;
    }
    updateIRQ();
    if (message) {
        if ((msixControl & 0x8000) && irqLevel) irq(true);
        replayPending();
    }
}
bool VirtioPciDevice::contains(uint64_t address, unsigned width) const {
    return (command & 2) && bar >= 0x10000000 && bar < 0x3f000000 &&
           address >= bar && address - bar < barSize() && width <= barSize() - (address - bar);
}
void VirtioPciDevice::irq(bool level, unsigned queue) {
    irqLevel = level;
    updateIRQ();
    if (level && (msixControl & 0x8000)) {
        auto causes = readReg(0x60);
        if (causes & 2) notifyVector(configVector);
        if ((causes & 1) && queue < queueVectors.size()) notifyVector(queueVectors[queue]);
        // MSI-X is edge based. Retire the backend latch after recording masked
        // messages in the PBA, so later completions can signal independently.
        if (causes) writeReg(0x64, causes);
    }
}
void VirtioPciDevice::updateIRQ() {
    interrupt(irqLevel && !(command & 0x400) && !(msixControl & 0x8000));
}
void VirtioPciDevice::notifyVector(uint16_t vector) {
    if (vector >= vectorTable.size()) return;
    pendingBits[vector / 8] |= uint8_t(1u << (vector % 8));
    replayVector(vector);
}
void VirtioPciDevice::replayVector(unsigned vector) {
    auto &entry = vectorTable[vector];
    auto bit = uint8_t(1u << (vector % 8));
    if (!(msixControl & 0x8000) || (msixControl & 0x4000) || !(command & 4) ||
        (entry[12] & 1) || !(pendingBits[vector / 8] & bit)) return;
    uint64_t address = 0;
    uint32_t data = 0;
    for (unsigned i = 0; i < 8; ++i) address |= uint64_t(entry[i]) << (i * 8);
    for (unsigned i = 0; i < 4; ++i) data |= uint32_t(entry[8 + i]) << (i * 8);
    message(address, data);
    pendingBits[vector / 8] &= uint8_t(~bit);
    ++messageInterrupts;
}
void VirtioPciDevice::replayPending() {
    for (unsigned i = 0; i < vectorTable.size(); ++i) replayVector(i);
}
uint64_t VirtioPciDevice::commonRead(unsigned offset, unsigned width) const {
    access(offset, width, 56);
    std::array<uint8_t, 256> common{};
    put(common, 0, readReg(0x14), 4); put(common, 4, readReg(0x10), 4);
    put(common, 8, readReg(0x24), 4); put(common, 12, readReg(0x20), 4);
    put(common, 16, configVector, 2); put(common, 18, queueCount, 2);
    put(common, 20, readReg(0x70), 1); put(common, 22, readReg(0x30), 2);
    put(common, 24, readReg(0x38), 2);
    put(common, 26, readReg(0x30) < queueVectors.size() ? queueVectors[readReg(0x30)] : 0xffff, 2); put(common, 28, readReg(0x44), 2);
    put(common, 30, readReg(0x30), 2);
    for (unsigned n = 0; n < 3; ++n) {
        unsigned reg = 0x80 + n * 0x10;
        put(common, 32 + n * 8, readReg(reg) | (readReg(reg + 4) << 32), 8);
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= uint64_t(common[offset + i]) << (i * 8);
    return value;
}
void VirtioPciDevice::commonWrite(unsigned offset, uint64_t value, unsigned width) {
    access(offset, width, 56);
    if (offset == 0 && width == 4) writeReg(0x14, value);
    else if (offset == 8 && width == 4) writeReg(0x24, value);
    else if (offset == 12 && width == 4) writeReg(0x20, value);
    else if (offset == 20 && width == 1) {
        writeReg(0x70, value);
        if (value == 0) {
            writeReg(0x38, readReg(0x34));
            configVector = 0xffff;
            std::fill(queueVectors.begin(), queueVectors.end(), 0xffff);
            pendingBits.fill(0);
        }
    }
    else if (message && offset == 16 && width == 2)
        configVector = value < vectorTable.size() ? uint16_t(value) : 0xffff;
    else if (message && offset == 26 && width == 2) {
        auto queue = readReg(0x30);
        if (queue < queueVectors.size()) queueVectors[queue] = value < vectorTable.size() ? uint16_t(value) : 0xffff;
    }
    else if (offset == 22 && width == 2) writeReg(0x30, value);
    else if (offset == 24 && width == 2) writeReg(0x38, value);
    else if (offset == 28 && width == 2) writeReg(0x44, value);
    else if (offset >= 32 && offset < 56 && (width == 4 || width == 8)) {
        unsigned reg = 0x80 + ((offset - 32) / 8) * 0x10 + ((offset - 32) % 8);
        writeReg(reg, uint32_t(value));
        if (width == 8) writeReg(reg + 4, value >> 32);
    }
}
uint64_t VirtioPciDevice::read(uint64_t address, unsigned width) {
    if (!contains(address, width)) throw std::runtime_error("Virtio PCI BAR is not enabled/mapped");
    unsigned offset = address - bar;
    access(offset, width, barSize());
    if (message && offset >= 0x4000 && offset < 0x4000 + vectorTable.size() * 16 &&
        width <= 0x4000 + vectorTable.size() * 16 - offset) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) {
            auto at = offset + i - 0x4000;
            value |= uint64_t(vectorTable[at / 16][at % 16]) << (i * 8);
        }
        return value;
    }
    if (message && offset >= 0x5000 && offset < 0x5020 && width <= 0x5020 - offset) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= uint64_t(pendingBits[offset - 0x5000 + i]) << (i * 8);
        return value;
    }
    if (offset < 56) return commonRead(offset, width);
    if (offset == 0x2000 && width == 1) {
        uint64_t value = readReg(0x60); writeReg(0x64, value); return value;
    }
    if (offset >= 0x3000 && offset < 0x3040) return readReg(0x100 + offset - 0x3000, width);
    return 0;
}
void VirtioPciDevice::write(uint64_t address, uint64_t value, unsigned width) {
    if (!contains(address, width)) throw std::runtime_error("Virtio PCI BAR is not enabled/mapped");
    unsigned offset = address - bar;
    access(offset, width, barSize());
    if (message && offset >= 0x4000 && offset < 0x4000 + vectorTable.size() * 16 &&
        width <= 0x4000 + vectorTable.size() * 16 - offset) {
        for (unsigned i = 0; i < width; ++i) {
            auto at = offset + i - 0x4000;
            auto byte = uint8_t(value >> (i * 8));
            vectorTable[at / 16][at % 16] = at % 16 < 12 ? byte : at % 16 == 12 ? byte & 1 : 0;
        }
        replayPending();
    }
    else if (offset < 56) commonWrite(offset, value, width);
    else if (offset >= 0x1000 && offset < 0x1000 + queueCount * 4u && (width == 2 || width == 4) && (command & 4))
        writeReg(0x50, value & 0xffff);
}
} // namespace pc
