#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pc {
// QEMU Firmware Configuration (fw_cfg) MMIO/DMA interface, including v2.9 DMA writes.
// https://www.qemu.org/docs/master/specs/fw_cfg.html
// Original device implementation; see arm_fw_cfg.cpp for wire formats and host policy.
class ArmFwCfg {
  public:
    // Bind RAM and CPU topology used by configuration items and DMA.
    ArmFwCfg(std::span<uint8_t> ram, uint64_t ramBase, unsigned cpuCount = 1);
    // Publish one named directory item, optionally writable through DMA.
    uint16_t add_file(std::string name, std::vector<uint8_t> bytes, bool writable = false);
    // Stream item bytes or read the DMA-interface discovery signature.
    uint64_t read_register(unsigned offset, unsigned width);
    // Select an item or submit a DMA descriptor address.
    void write_register(unsigned offset, uint64_t value, unsigned width);
    // Expose a named item by its directory selector for host-side consumers.
    std::span<const uint8_t> file_data(uint16_t selector) const;

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    struct Item {
        std::string name;
        std::vector<uint8_t> bytes;
        bool writable;
    };
    std::vector<Item> items;
    std::vector<uint8_t> directory;
    std::span<uint8_t> ram;
    uint64_t base, cursor = 0, dmaAddress = 0;
    uint16_t selected = 0;
    std::array<uint8_t, 2> cpus{};
    // Resolve the current selector, including protocol discovery items.
    std::span<const uint8_t> selected_item_data() const;
    // Validate a guest-physical DMA range and return the corresponding RAM span.
    std::span<uint8_t> guest_memory(uint64_t address, uint64_t length);
    // Execute one synchronous transfer and publish completion in its descriptor.
    void perform_dma_transfer();
};
} // namespace pc
