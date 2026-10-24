#pragma once
#include "disk.hpp"
#include <array>
#include <functional>
#include <span>
namespace pc {
// Bounded NVMe 1.1 PCI controller: one namespace, 4 KiB pages, legacy INTx.
// No MSI/MSI-X, SGL, metadata, discard, multipath or background execution.
class Nvme {
  public:
    Nvme(Disk &disk, std::span<uint8_t> ram, uint64_t ramBase, std::function<void(bool)> irq);
    uint64_t configRead(uint64_t offset, unsigned width) const;
    void configWrite(uint64_t offset, uint64_t value, unsigned width);
    bool contains(uint64_t address, unsigned width) const;
    uint64_t read(uint64_t address, unsigned width) const;
    void write(uint64_t address, uint64_t value, unsigned width);
    uint64_t requests = 0;
    std::string diagnostics() const;
  private:
    static constexpr unsigned queueCount = 16, maxEntries = 256;
    struct Queue {
        uint64_t base = 0;
        unsigned size = 0, head = 0, tail = 0, pending = 0, cq = 0;
        bool phase = true, irq = false;
    };
    Disk &disk;
    std::span<uint8_t> ram;
    uint64_t ramBase;
    std::function<void(bool)> interrupt;
    std::array<uint8_t, 256> config{};
    std::array<Queue, queueCount> sq{}, cq{};
    uint64_t asq = 0, acq = 0;
    uint32_t bar = 0, cc = 0, csts = 0, aqa = 0, interruptMask = 0;
    uint16_t pciCommand = 0;
    bool probe = false;
    uint32_t queueFeature = 0x000e000e, asyncConfig = 0, coalescing = 0;
    uint64_t adminRequests = 0, ioRequests = 0, errors = 0, resets = 0;
    uint32_t lastError = 0;
    unsigned asyncPending = 0;
    std::span<uint8_t> memory(uint64_t address, size_t bytes);
    void updateIRQ();
    void enable();
    void process(unsigned qid);
    uint16_t execute(unsigned qid, std::span<const uint8_t> cmd, uint32_t &result, bool &defer);
    bool transfer(std::span<const uint8_t> cmd, std::span<uint8_t> data, bool toGuest);
};
} // namespace pc
