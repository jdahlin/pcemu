#pragma once
#include <array>
#include <cstdint>
namespace pc {
// Guest-visible PCI function for the original VGA/DISPI device. The PCI bus
// owns configuration-mechanism addressing; this device owns its config/BARs.
struct VgaPciDevice {
    std::array<uint8_t, 256> config{};
    bool barProbe = false, romProbe = false;
    VgaPciDevice();
    uint32_t read(unsigned offset, unsigned width) const;
    void write(unsigned offset, uint32_t value, unsigned width);
};
} // namespace pc
