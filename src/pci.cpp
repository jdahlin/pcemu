// PCI (Peripheral Component Interconnect) configuration mechanism 1 lets PC
// firmware discover devices and configure their address windows. Software writes
// CONFIG_ADDRESS at I/O port 0xcf8, then transfers configuration bytes through
// CONFIG_DATA at 0xcfc..0xcff. Bus/device/function (BDF) selects an endpoint;
// the endpoint owns its registers, BARs (Base Address Registers) and probe state.
// This bus routes only the board's VGA function at bus 0, device 2, function 0.
// Unattached functions read as all ones and ignore writes. BIOS helpers may use
// the direct configuration methods without going through the address latch.
//
// IMPLEMENTATION NOTE: reset installs the board's firmware-initialized display
// state. There is no bridge hierarchy, hotplug or electrical PCI transaction model.
// COMPATIBILITY: CONFIG_ADDRESS reads return its full latch even for narrow
// accesses; the caller selects the requested width. The specification only
// defines DWORD accesses to this register; narrower bus pass-through is absent.
// PCI Local Bus Specification 3.0, 3.2.2.3.2, Figure 3-2.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=50

#include "pci.hpp"
namespace pc {
namespace {
// Configuration mechanism 1 uses these two DWORD I/O locations (Figure 3-2).
enum Register : uint16_t {
    ConfigurationAddressRegister = 0xcf8,
    ConfigurationDataRegister = 0xcfc,
};
// CONFIG_ADDRESS bit 31 enables configuration translation through CONFIG_DATA.
constexpr uint32_t ConfigurationEnable = 1u << 31;
// Only Enable[31], Bus[23:16], Device[15:11], Function[10:8] and Register[7:2]
// are writable; reserved bits and the low two alignment bits read zero.
constexpr uint32_t ConfigurationAddressWritableMask = 0x80fffffc;
// The packed BDF occupies CONFIG_ADDRESS bits [23:8].
constexpr unsigned BusDeviceFunctionShift = 8;
constexpr uint32_t BusDeviceFunctionMask = 0xffff;
// The conventional configuration space register is DWORD-aligned, bits [7:2].
constexpr uint32_t RegisterOffsetMask = 0xfc;
// A CONFIG_DATA transfer must stay within its four-byte I/O window.
constexpr unsigned ConfigurationDataBytes = 4;
// Missing functions are discovered when Vendor ID reads 0xffff; return ones
// across the host result width so byte/word callers see the same absent value.
constexpr uint32_t AbsentConfigurationValue = 0xffffffffu;
// IMPLEMENTATION NOTE: Board wiring: bus 0, device 2, function 0, packed as
// (bus << 8) | (device << 3) | function. This address is not mandated by PCI.
constexpr uint16_t DisplayBusDeviceFunction = 0x10;
} // namespace
// Read the selected endpoint configuration bytes; absent BDFs return all ones.
// PCI 3.0, 3.2.2.3.2: configuration mechanism 1.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=50
uint32_t PCI::read_configuration(uint16_t bdf, unsigned offset, unsigned width) const {
    return bdf == DisplayBusDeviceFunction ? display_.read(offset, width)
                                           : AbsentConfigurationValue;
}
// Forward a configuration write to the wired display; ignore absent functions.
// PCI 3.0, 3.2.2.3.2: configuration mechanism 1.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=50
void PCI::write_configuration(uint16_t bdf, unsigned offset, uint32_t value, unsigned width) {
    if (bdf == DisplayBusDeviceFunction)
        display_.write(offset, value, width);
}
// Read CONFIG_ADDRESS or translate a bounded CONFIG_DATA access using its latch.
// PCI 3.0, 3.2.2.3.2: configuration mechanism 1.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=50
uint32_t PCI::read_io_register(uint16_t port, unsigned width) const {
    if (port == Register::ConfigurationAddressRegister)
        return address;
    if (!(address & ConfigurationEnable) || port < Register::ConfigurationDataRegister ||
        unsigned(port - Register::ConfigurationDataRegister) + width > ConfigurationDataBytes)
        return AbsentConfigurationValue;
    return read_configuration(
        (address >> BusDeviceFunctionShift) & BusDeviceFunctionMask,
        (address & RegisterOffsetMask) + (port - Register::ConfigurationDataRegister), width);
}
// Latch a DWORD CONFIG_ADDRESS write or forward enabled CONFIG_DATA bytes.
// PCI 3.0, 3.2.2.3.2: configuration mechanism 1.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=50
void PCI::write_io_register(uint16_t port, uint32_t value, unsigned width) {
    if (port == Register::ConfigurationAddressRegister && width == ConfigurationDataBytes) {
        address = value & ConfigurationAddressWritableMask;
        return;
    }
    if ((address & ConfigurationEnable) && port >= Register::ConfigurationDataRegister &&
        unsigned(port - Register::ConfigurationDataRegister) + width <= ConfigurationDataBytes)
        write_configuration((address >> BusDeviceFunctionShift) & BusDeviceFunctionMask,
                            (address & RegisterOffsetMask) +
                                (port - Register::ConfigurationDataRegister),
                            value, width);
}
} // namespace pc
