#pragma once
#include "vga_pci.hpp"
#include <cstdint>
namespace pc {
// PCI configuration mechanism 1, with the existing fixed display attachment.
// Bus identity/address decoding is separate from the endpoint register model.
class PCI {
  public:
    uint32_t address = 0;
    void reset() { address = 0; display_ = VgaPciDevice{}; }
    VgaPciDevice &display() { return display_; }
    const VgaPciDevice &display() const { return display_; }
    uint32_t read(uint16_t bdf, unsigned offset, unsigned width) const;
    void write(uint16_t bdf, unsigned offset, uint32_t value, unsigned width);
    uint32_t in(uint16_t port, unsigned width) const;
    void out(uint16_t port, uint32_t value, unsigned width);
  private:
    friend class SnapshotCodec;
    VgaPciDevice display_;
};
} // namespace pc
