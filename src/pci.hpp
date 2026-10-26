#pragma once
#include "vga_pci.hpp"
#include <cstdint>
namespace pc {
// PCI configuration mechanism 1, with the existing fixed display attachment.
// Bus identity/address decoding is separate from the endpoint register model.
class PCI {
  public:
    uint32_t address = 0;
    // Restore the address latch and firmware-initialized display configuration.
    // IMPLEMENTATION NOTE: Board initialization around PCI configuration mechanism 1.
    // https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=50
    void reset() {
        address = 0;
        display_ = VgaPciDevice{};
    }
    // Expose the attached display configuration for host inspection/snapshots.
    // IMPLEMENTATION NOTE: Host endpoint access, not a PCI transaction.
    // https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=50
    VgaPciDevice &display() {
        return display_;
    }
    // Inspect the attached display without changing guest state.
    // IMPLEMENTATION NOTE: Host endpoint access, not a PCI transaction.
    // https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=50
    const VgaPciDevice &display() const {
        return display_;
    }
    // Read configuration bytes at a packed bus/device/function address.
    uint32_t read_configuration(uint16_t bdf, unsigned offset, unsigned width) const;
    // Write configuration bytes to the selected endpoint.
    void write_configuration(uint16_t bdf, unsigned offset, uint32_t value, unsigned width);
    // Read the configuration address latch or selected data bytes.
    uint32_t read_io_register(uint16_t port, unsigned width) const;
    // Write the configuration address latch or selected data bytes.
    void write_io_register(uint16_t port, uint32_t value, unsigned width);

  private:
    friend class SnapshotCodec;
    VgaPciDevice display_;
};
} // namespace pc
