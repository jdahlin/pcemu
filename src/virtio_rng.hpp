#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace pc {
// Virtio entropy device: one modern split queue, host CSPRNG, no device features.
// Processing runs with the vCPU stopped; DMA and descriptor walks are bounded.
class VirtioRng {
  public:
    using Entropy = std::function<void(std::span<uint8_t>)>;
    VirtioRng(std::span<uint8_t> ram, uint64_t ramBase,
              std::function<void(bool)> interrupt, Entropy entropy = {});
    uint64_t read(unsigned offset, unsigned width = 4) const;
    void write(unsigned offset, uint64_t value, unsigned width = 4);
    const std::string &error() const { return failure; }
    uint64_t bytesSupplied = 0;
  private:
    std::span<uint8_t> ram;
    uint64_t ramBase;
    std::function<void(bool)> interrupt;
    Entropy entropy;
    uint32_t featureSelect = 0, driverSelect = 0, queueSelect = 0;
    uint32_t queueSize = 0, ready = 0, status = 0, interruptStatus = 0;
    uint64_t driverFeatures = 0, descriptors = 0, available = 0, used = 0;
    uint16_t lastAvailable = 0, usedIndex = 0;
    std::string failure;
    static constexpr uint64_t features = 1ull << 32;
    uint8_t *memory(uint64_t address, uint64_t length) const;
    uint64_t get(uint64_t address, unsigned width) const;
    void put(uint64_t address, uint64_t value, unsigned width);
    void reset();
    void validateQueue() const;
    void notify();
    uint32_t request(uint16_t head);
    void broken(const std::string &reason);
};
} // namespace pc
