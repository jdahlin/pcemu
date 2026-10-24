#pragma once
#include "virtio_blk.hpp"
#include <array>
#include <functional>
namespace pc {
// Modern virtio PCI endpoint, configuration via ECAM, one 16 KiB BAR.
// Common/device/notify/ISR capabilities; legacy INTx, no MSI-X or transitional I/O.
class VirtioPciDevice {
  public:
    VirtioPciDevice(VirtioBlock &block, std::function<void(bool)> interrupt);
    using Reader = std::function<uint64_t(unsigned, unsigned)>;
    using Writer = std::function<void(unsigned, uint64_t, unsigned)>;
    VirtioPciDevice(Reader read, Writer write, uint16_t deviceId, uint32_t classCode,
                   uint16_t queues, std::function<void(bool)> interrupt);
    uint64_t configRead(uint64_t offset, unsigned width) const;
    void configWrite(uint64_t offset, uint64_t value, unsigned width);
    bool contains(uint64_t address, unsigned width) const;
    uint64_t read(uint64_t address, unsigned width);
    void write(uint64_t address, uint64_t value, unsigned width);
    void irq(bool level);
  private:
    Reader reader;
    Writer writer;
    uint16_t queueCount;
    uint64_t readReg(unsigned offset, unsigned width = 4) const { return reader(offset, width); }
    void writeReg(unsigned offset, uint64_t value, unsigned width = 4) { writer(offset, value, width); }
    std::function<void(bool)> interrupt;
    std::array<uint8_t, 256> config{};
    uint32_t bar = 0;
    bool barProbe = false, irqLevel = false;
    uint16_t command = 0;
    void updateIRQ();
    uint64_t commonRead(unsigned offset, unsigned width) const;
    void commonWrite(unsigned offset, uint64_t value, unsigned width);
};
// Existing block-device call sites retain their descriptive name.
using VirtioPciBlock = VirtioPciDevice;
} // namespace pc
