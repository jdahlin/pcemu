#pragma once
#include <array>
#include <cstdint>
namespace pc {
// Guest-visible PCI function for the original VGA/DISPI device. The PCI bus
// owns configuration-mechanism addressing; this device owns its config/BARs.
struct VgaPciDevice {
    std::array<uint8_t, 256> config{};
    bool barProbe = false, romProbe = false;
    // Install the board’s firmware-initialized PCI identity and fixed windows.
    VgaPciDevice();
    // Read little-endian header bytes, including active size-probe responses.
    uint32_t read_configuration(unsigned offset, unsigned width) const;
    // Handle size probes and Command enable bits; ignore other writes.
    void write_configuration(unsigned offset, uint32_t value, unsigned width);
};
} // namespace pc
