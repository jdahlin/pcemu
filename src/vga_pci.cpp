// The VGA PCI function is the discovery/configuration facade for the PC display.
// Firmware reads its vendor/device and class codes, probes the framebuffer BAR
// (Base Address Register) and option-ROM size, then uses VGA/DISPI display I/O.
// Configuration mechanism 1 belongs to PCI; rendering and actual memory/I/O
// decoding belong to the board and VGA model. This object stores only the
// 256-byte conventional configuration header and temporary size-probe latches.
//
// COMPATIBILITY: The identity matches the Bochs/QEMU Standard VGA convention,
// but the framebuffer is fixed at 0xe0000000 and ROM shadow at 0x000c0000.
// Construction represents firmware-initialized state, with I/O and memory enabled,
// rather than PCI electrical reset state. Only the low Command bits are writable;
// size probes do not relocate either window. There is no interrupt pin.
// FIXME: Implement BAR relocation and gate board decoding on Command bits before
// claiming PCI address-map conformance. Current fixed mappings are retained.
// PCI Local Bus 3.0, 6.2.1 / 6.2.2 / 6.2.5; QEMU Standard VGA, PCI spec.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=216
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=225
// https://www.qemu.org/docs/master/specs/standard-vga.html#pci-spec

#include "vga_pci.hpp"
namespace pc {
namespace {
// Byte offsets within a PCI type-0 configuration header (PCI 3.0, section 6.1).
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=214
enum Register : unsigned {
    // Two-byte vendor identifier; 0xffff would mean no function is present.
    VendorIdRegister = 0x00,
    // Two-byte device identifier assigned within the vendor's namespace.
    DeviceIdRegister = 0x02,
    // Command low byte: bit 0 enables I/O, bit 1 enables memory decoding.
    CommandRegister = 0x04,
    // Upper byte of the three-byte class code; lower bytes remain zero (VGA).
    BaseClassRegister = 0x0b,
    // First 32-bit memory Base Address Register: the display's framebuffer.
    FramebufferBaseAddressRegister = 0x10,
    // 32-bit expansion-ROM base and enable bit.
    ExpansionRomBaseAddressRegister = 0x30,
    // Firmware's platform interrupt routing number, separate from Interrupt Pin.
    InterruptLineRegister = 0x3c,
};
// Bochs/QEMU VGA compatibility identity, written little-endian into the header.
// https://www.qemu.org/docs/master/specs/standard-vga.html#pci-spec
constexpr uint16_t VgaVendorId = 0x1234;
constexpr uint16_t VgaDeviceId = 0x1111;
// Class 03h is display; subclass/interface 00h identify VGA compatibility.
constexpr uint8_t DisplayBaseClass = 0x03;
// PCI Command register bits 0/1; this facade stores these two enable requests.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=218
constexpr uint8_t IoSpaceEnable = 1u << 0;
constexpr uint8_t MemorySpaceEnable = 1u << 1;
// IMPLEMENTATION NOTE: Board-assigned fixed framebuffer base, not a PCI requirement.
constexpr uint32_t FramebufferBaseAddress = 0xe0000000;
// BAR size response: zeros in low 24 bits request a 16 MiB aligned memory window.
constexpr uint32_t FramebufferSizeProbeValue = 0xff000000;
// IMPLEMENTATION NOTE: Board ROM shadow address in conventional PC memory.
constexpr uint32_t ExpansionRomBaseAddress = 0x000c0000;
// Expansion ROM register bit 0 enables its address window.
constexpr uint32_t ExpansionRomEnable = 1u << 0;
// Expansion ROM register bits [31:11] hold the address; bits [10:1] are reserved.
constexpr uint32_t ExpansionRomAddressMask = 0xfffff800;
// ROM size response: zeros in low 16 bits request a 64 KiB aligned ROM window.
constexpr uint32_t ExpansionRomSizeProbeValue = 0xffff0000;
// Writing all ones to BAR0 starts a size probe; absent/out-of-range reads also
// return ones. These have distinct names because they serve different purposes.
constexpr uint32_t BaseAddressSizeProbeRequest = 0xffffffffu;
constexpr uint32_t UnimplementedConfigurationValue = 0xffffffffu;
// COMPATIBILITY: Retained firmware interrupt-line sentinel. Interrupt Pin stays
// zero in the zero-filled header, which is what indicates no pin is connected.
constexpr uint8_t UnassignedInterruptLine = 0xff;
// PCI BARs/ROM base are DWORD fields; configuration byte order is little-endian.
constexpr unsigned AddressRegisterBytes = 4;
constexpr unsigned BitsPerByte = 8;
} // namespace
// Build the board's firmware-initialized identity, fixed address map and no-IRQ header.
// PCI 3.0, 6.2.1/6.2.2 and 6.2.5; identity follows QEMU Standard VGA's PCI convention.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=216
// https://www.qemu.org/docs/master/specs/standard-vga.html#pci-spec
VgaPciDevice::VgaPciDevice() {
    config[Register::VendorIdRegister] = uint8_t(VgaVendorId);
    config[Register::VendorIdRegister + 1] = uint8_t(VgaVendorId >> BitsPerByte);
    config[Register::DeviceIdRegister] = uint8_t(VgaDeviceId);
    config[Register::DeviceIdRegister + 1] = uint8_t(VgaDeviceId >> BitsPerByte);
    config[Register::CommandRegister] = IoSpaceEnable | MemorySpaceEnable;
    config[Register::BaseClassRegister] = DisplayBaseClass;
    for (unsigned byte = 0; byte < AddressRegisterBytes; ++byte) {
        config[Register::FramebufferBaseAddressRegister + byte] =
            uint8_t(FramebufferBaseAddress >> (byte * BitsPerByte));
        config[Register::ExpansionRomBaseAddressRegister + byte] =
            uint8_t((ExpansionRomBaseAddress | ExpansionRomEnable) >> (byte * BitsPerByte));
    }
    config[Register::InterruptLineRegister] = UnassignedInterruptLine;
}
// Read little-endian configuration bytes, substituting active BAR/ROM size probes.
// PCI 3.0, 6.2.5.1 Address Maps and 6.2.5.2 Expansion ROMs.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=227
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=228
uint32_t VgaPciDevice::read_configuration(unsigned offset, unsigned width) const {
    if (offset + width > config.size())
        return UnimplementedConfigurationValue;
    uint32_t result = 0;
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        uint8_t value = config[at];
        if (barProbe && at >= Register::FramebufferBaseAddressRegister &&
            at < Register::FramebufferBaseAddressRegister + AddressRegisterBytes)
            value = uint8_t(FramebufferSizeProbeValue >>
                            ((at - Register::FramebufferBaseAddressRegister) * BitsPerByte));
        if (romProbe && at >= Register::ExpansionRomBaseAddressRegister &&
            at < Register::ExpansionRomBaseAddressRegister + AddressRegisterBytes)
            value = uint8_t(ExpansionRomSizeProbeValue >>
                            ((at - Register::ExpansionRomBaseAddressRegister) * BitsPerByte));
        result |= uint32_t(value) << (i * BitsPerByte);
    }
    return result;
}
// Recognize DWORD BAR/ROM size probes and store Command I/O/memory enables.
// COMPATIBILITY: Other bytes, including address relocation writes, remain fixed.
// PCI 3.0, 6.2.2 Table 6-1 and 6.2.5 address-map probing.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=218
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=227
void VgaPciDevice::write_configuration(unsigned offset, uint32_t value, unsigned width) {
    if (offset + width > config.size())
        return;
    if (offset == Register::FramebufferBaseAddressRegister && width == AddressRegisterBytes) {
        barProbe = value == BaseAddressSizeProbeRequest;
        return;
    }
    if (offset == Register::ExpansionRomBaseAddressRegister && width == AddressRegisterBytes) {
        romProbe = (value & ExpansionRomAddressMask) == ExpansionRomAddressMask;
        return;
    }
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        if (at == Register::CommandRegister)
            config[at] = uint8_t(value >> (i * BitsPerByte)) & (IoSpaceEnable | MemorySpaceEnable);
        // Other fields describe fixed wiring. BAR relocation isn't implemented.
    }
}
} // namespace pc
