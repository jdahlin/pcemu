#include "vga_pci.hpp"
namespace pc {
VgaPciDevice::VgaPciDevice() {
    config[0] = 0x34;
    config[1] = 0x12;
    config[2] = 0x11;
    config[3] = 0x11;
    config[4] = 3;       // I/O and memory decoding enabled by firmware.
    config[0xb] = 3;     // base class display, subclass VGA.
    config[0x13] = 0xe0; // BAR0, fixed 16 MiB framebuffer at E0000000.
    config[0x30] = 1;
    config[0x32] = 0x0c; // VGA option ROM shadow at C0000.
    config[0x3c] = 0xff; // no PCI interrupt pin; VGA uses no IRQ.
}
uint32_t VgaPciDevice::read(unsigned offset, unsigned width) const {
    if (offset + width > config.size())
        return 0xffffffffu;
    uint32_t result = 0;
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        uint8_t value = config[at];
        if (barProbe && at >= 0x10 && at < 0x14)
            value = uint8_t(0xff000000u >> ((at - 0x10) * 8));
        if (romProbe && at >= 0x30 && at < 0x34)
            value = uint8_t(0xffff0000u >> ((at - 0x30) * 8));
        result |= uint32_t(value) << (i * 8);
    }
    return result;
}
void VgaPciDevice::write(unsigned offset, uint32_t value, unsigned width) {
    if (offset + width > config.size())
        return;
    if (offset == 0x10 && width == 4) {
        barProbe = value == 0xffffffffu;
        return;
    }
    if (offset == 0x30 && width == 4) {
        romProbe = (value & 0xfffff800u) == 0xfffff800u;
        return;
    }
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        if (at == 4)
            config[at] = uint8_t(value >> (i * 8)) & 3;
        // Other fields describe fixed wiring. BAR relocation isn't implemented.
    }
}
} // namespace pc
