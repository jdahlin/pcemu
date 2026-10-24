#include "virtio_pci.hpp"
#include <stdexcept>
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
VirtioPciBlock::VirtioPciBlock(VirtioBlock &b, std::function<void(bool)> irq)
    : block(b), interrupt(std::move(irq)) {
    block.write(0x38, block.read(0x34));
    put(config, 0, 0x10421af4, 4);
    put(config, 6, 0x10, 2); // capabilities list
    put(config, 8, 0x01000001, 4); // mass storage/SCSI, revision 1
    put(config, 0x2c, 0x00421af4, 4);
    config[0x34] = 0x40;
    config[0x3c] = 0xff; config[0x3d] = 1;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned o = 0x40 + i * 0x14;
        config[o] = 9; // vendor-specific capability
        config[o + 1] = i == 3 ? 0 : o + 0x14;
        config[o + 2] = i == 1 ? 20 : 16;
        config[o + 3] = i + 1; // common, notify, ISR, device
        config[o + 4] = 0; // BAR0
        put(config, o + 8, i * 0x1000, 4);
        put(config, o + 12, i == 0 ? 56 : i == 1 ? 0x1000 : i == 2 ? 1 : 64, 4);
        if (i == 1) put(config, o + 16, 4, 4); // notify_off_multiplier
    }
}
uint64_t VirtioPciBlock::configRead(uint64_t offset, unsigned width) const {
    access(offset & 0xfff, width, 4096);
    if (offset >= 256) return mask(width); // only function 00:00.0, standard config space
    auto bytes = config;
    put(bytes, 4, command, 2);
    put(bytes, 6, 0x10 | (irqLevel ? 8 : 0), 2);
    put(bytes, 0x10, barProbe ? 0xffffc000 : bar, 4);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}
void VirtioPciBlock::configWrite(uint64_t offset, uint64_t value, unsigned width) {
    access(offset & 0xfff, width, 4096);
    if (offset >= 256) return;
    if (offset == 0x10 && width == 4) {
        barProbe = uint32_t(value) == 0xffffffff;
        if (!barProbe) bar = uint32_t(value) & 0xffffc000;
        return;
    }
    for (unsigned i = 0; i < width; ++i) {
        auto at = offset + i;
        uint8_t byte = value >> (i * 8);
        if (at == 4) command = (command & 0xff00) | (byte & 7);
        if (at == 5) command = (command & 0xff) | (uint16_t(byte & 4) << 8);
        if (at == 0xc || at == 0xd || at == 0x3c) config[at] = byte;
    }
    updateIRQ();
}
bool VirtioPciBlock::contains(uint64_t address, unsigned width) const {
    return (command & 2) && bar >= 0x10000000 && bar < 0x3f000000 &&
           address >= bar && address - bar < 0x4000 && width <= 0x4000 - (address - bar);
}
void VirtioPciBlock::irq(bool level) { irqLevel = level; updateIRQ(); }
void VirtioPciBlock::updateIRQ() { interrupt(irqLevel && !(command & 0x400)); }
uint64_t VirtioPciBlock::commonRead(unsigned offset, unsigned width) const {
    access(offset, width, 56);
    std::array<uint8_t, 256> common{};
    put(common, 0, block.read(0x14), 4); put(common, 4, block.read(0x10), 4);
    put(common, 8, block.read(0x24), 4); put(common, 12, block.read(0x20), 4);
    put(common, 16, 0xffff, 2); put(common, 18, 1, 2);
    put(common, 20, block.read(0x70), 1); put(common, 22, block.read(0x30), 2);
    put(common, 24, block.read(0x38), 2);
    put(common, 26, 0xffff, 2); put(common, 28, block.read(0x44), 2);
    for (unsigned n = 0; n < 3; ++n) {
        unsigned reg = 0x80 + n * 0x10;
        put(common, 32 + n * 8, block.read(reg) | (block.read(reg + 4) << 32), 8);
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= uint64_t(common[offset + i]) << (i * 8);
    return value;
}
void VirtioPciBlock::commonWrite(unsigned offset, uint64_t value, unsigned width) {
    access(offset, width, 56);
    if (offset == 0 && width == 4) block.write(0x14, value);
    else if (offset == 8 && width == 4) block.write(0x24, value);
    else if (offset == 12 && width == 4) block.write(0x20, value);
    else if (offset == 20 && width == 1) {
        block.write(0x70, value);
        if (value == 0) block.write(0x38, block.read(0x34));
    }
    else if (offset == 22 && width == 2) block.write(0x30, value);
    else if (offset == 24 && width == 2) block.write(0x38, value);
    else if (offset == 28 && width == 2) block.write(0x44, value);
    else if (offset >= 32 && offset < 56 && (width == 4 || width == 8)) {
        unsigned reg = 0x80 + ((offset - 32) / 8) * 0x10 + ((offset - 32) % 8);
        block.write(reg, uint32_t(value));
        if (width == 8) block.write(reg + 4, value >> 32);
    }
}
uint64_t VirtioPciBlock::read(uint64_t address, unsigned width) {
    if (!contains(address, width)) throw std::runtime_error("Virtio PCI BAR is not enabled/mapped");
    unsigned offset = address - bar;
    access(offset, width, 0x4000);
    if (offset < 56) return commonRead(offset, width);
    if (offset == 0x2000 && width == 1) {
        uint64_t value = block.read(0x60); block.write(0x64, value); return value;
    }
    if (offset >= 0x3000 && offset < 0x3040) return block.read(0x100 + offset - 0x3000, width);
    return 0;
}
void VirtioPciBlock::write(uint64_t address, uint64_t value, unsigned width) {
    if (!contains(address, width)) throw std::runtime_error("Virtio PCI BAR is not enabled/mapped");
    unsigned offset = address - bar;
    access(offset, width, 0x4000);
    if (offset < 56) commonWrite(offset, value, width);
    else if (offset == 0x1000 && (width == 2 || width == 4) && (command & 4))
        block.write(0x50, value & 0xffff);
}
} // namespace pc
