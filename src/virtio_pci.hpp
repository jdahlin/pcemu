#pragma once
#include "virtio_blk.hpp"
#include <array>
#include <functional>
namespace pc {
// Modern virtio-blk PCI endpoint, configuration via ECAM, one 16 KiB BAR.
// Common/device/notify/ISR capabilities; legacy INTx, no MSI-X or transitional I/O.
class VirtioPciBlock {
  public:
    VirtioPciBlock(VirtioBlock &block, std::function<void(bool)> interrupt);
    uint64_t configRead(uint64_t offset, unsigned width) const;
    void configWrite(uint64_t offset, uint64_t value, unsigned width);
    bool contains(uint64_t address, unsigned width) const;
    uint64_t read(uint64_t address, unsigned width);
    void write(uint64_t address, uint64_t value, unsigned width);
    void irq(bool level);
  private:
    VirtioBlock &block;
    std::function<void(bool)> interrupt;
    std::array<uint8_t, 256> config{};
    uint32_t bar = 0;
    bool barProbe = false, irqLevel = false;
    uint16_t command = 0;
    void updateIRQ();
    uint64_t commonRead(unsigned offset, unsigned width) const;
    void commonWrite(unsigned offset, uint64_t value, unsigned width);
};
} // namespace pc
