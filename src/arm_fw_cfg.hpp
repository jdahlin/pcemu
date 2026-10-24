#pragma once
#include <cstdint>
#include <array>
#include <span>
#include <string>
#include <vector>

namespace pc {
// Original implementation of the documented fw_cfg MMIO/DMA wire interface.
class ArmFwCfg {
  public:
    ArmFwCfg(std::span<uint8_t> ram, uint64_t ramBase, unsigned cpuCount = 1);
    uint16_t addFile(std::string name, std::vector<uint8_t> bytes, bool writable = false);
    uint64_t read(unsigned offset, unsigned width);
    void write(unsigned offset, uint64_t value, unsigned width);
    std::span<const uint8_t> file(uint16_t selector) const;
  private:
    struct Item { std::string name; std::vector<uint8_t> bytes; bool writable; };
    std::vector<Item> items;
    std::vector<uint8_t> directory;
    std::span<uint8_t> ram;
    uint64_t base, cursor = 0, dmaAddress = 0;
    uint16_t selected = 0;
    std::array<uint8_t, 2> cpus{};
    std::span<const uint8_t> data() const;
    std::span<uint8_t> memory(uint64_t address, uint64_t length);
    void dma();
};
} // namespace pc
