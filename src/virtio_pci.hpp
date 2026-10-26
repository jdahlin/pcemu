#pragma once
#include "virtio_blk.hpp"
#include <array>
#include <vector>
#include <functional>
namespace pc {
// Modern virtio PCI endpoint with common/device/notify/ISR capabilities.
// INTx uses a 16 KiB BAR; an optional MSI sender enables MSI-X and a 32 KiB BAR.
class VirtioPciDevice {
  public:
    using Message = std::function<void(uint64_t, uint32_t)>;
    VirtioPciDevice(VirtioBlock &block, std::function<void(bool)> interrupt, Message message = {});
    using Reader = std::function<uint64_t(unsigned, unsigned)>;
    using Writer = std::function<void(unsigned, uint64_t, unsigned)>;
    VirtioPciDevice(Reader read, Writer write, uint16_t deviceId, uint32_t classCode,
                   uint16_t queues, std::function<void(bool)> interrupt, Message message = {});
    uint64_t configRead(uint64_t offset, unsigned width) const;
    void configWrite(uint64_t offset, uint64_t value, unsigned width);
    bool contains(uint64_t address, unsigned width) const;
    uint64_t read(uint64_t address, unsigned width);
    void write(uint64_t address, uint64_t value, unsigned width);
    void irq(bool level, unsigned queue = 0);
    uint64_t messageInterrupts = 0;
    bool msixEnabled() const { return (msixControl & 0x8000) != 0; }
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
    Message message;
    uint16_t msixControl = 0, configVector = 0xffff;
    std::vector<uint16_t> queueVectors;
    std::vector<std::array<uint8_t, 16>> vectorTable;
    std::array<uint8_t, 32> pendingBits{};
    unsigned barSize() const { return message ? 0x8000 : 0x4000; }
    void notifyVector(uint16_t vector);
    void replayVector(unsigned vector);
    void replayPending();
    void updateIRQ();
    uint64_t commonRead(unsigned offset, unsigned width) const;
    void commonWrite(unsigned offset, uint64_t value, unsigned width);
};
// Existing block-device call sites retain their descriptive name.
using VirtioPciBlock = VirtioPciDevice;
} // namespace pc
