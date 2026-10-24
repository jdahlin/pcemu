#pragma once
#include "disk.hpp"
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace pc {
// Modern virtio-mmio v2, one split virtqueue. Device processing runs while the
// single vCPU is stopped. No host pointers or unvalidated DMA reach disk I/O.
class VirtioBlock {
  public:
    VirtioBlock(Disk &disk, std::span<uint8_t> ram, uint64_t ramBase,
                std::function<void(bool)> interrupt);
    uint64_t read(unsigned offset, unsigned width = 4) const;
    void write(unsigned offset, uint64_t value, unsigned width = 4);
    uint64_t requests = 0, bytesRead = 0, bytesWritten = 0;
    const std::string &error() const { return failure; }
  private:
    struct Buffer { uint64_t address; uint32_t length; bool writable; };
    Disk &disk;
    std::span<uint8_t> ram;
    uint64_t ramBase;
    std::function<void(bool)> interrupt;
    uint32_t featureSelect = 0, driverSelect = 0, queueSelect = 0;
    uint32_t queueSize = 0, ready = 0, status = 0, interruptStatus = 0;
    uint64_t driverFeatures = 0, descriptors = 0, available = 0, used = 0;
    uint16_t lastAvailable = 0, usedIndex = 0;
    std::string failure;
    static constexpr uint64_t features = (1ull << 32) | (1ull << 1) | (1ull << 2) | (1ull << 6) | (1ull << 9);
    uint8_t *memory(uint64_t address, uint64_t length) const;
    uint64_t get(uint64_t address, unsigned width) const;
    void put(uint64_t address, uint64_t value, unsigned width);
    void reset();
    void validateQueue() const;
    void notify();
    uint32_t request(uint16_t head);
    std::vector<Buffer> chain(uint16_t head) const;
    void copy(const std::vector<Buffer> &buffers, bool writable, size_t skip,
              std::span<uint8_t> data, bool toGuest);
    void broken(const std::string &reason);
};
} // namespace pc
