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
    // Bind disk/RAM/interrupt callback and construct the retained PCI discovery identity.
    Nvme(Disk &disk, std::span<uint8_t> ram, uint64_t ramBase, std::function<void(bool)> irq);
    // Read PCI header/BAR size-probe and aggregate completion-interrupt status.
    uint64_t configRead(uint64_t offset, unsigned width) const;
    // Apply BAR/Command and platform routing writes, then refresh INTx.
    void configWrite(uint64_t offset, uint64_t value, unsigned width);
    // Test enabled BAR decoding inside this board’s PCI memory aperture.
    bool contains(uint64_t address, unsigned width) const;
    // Synthesize controller register values; unused BAR registers read zero.
    uint64_t read(uint64_t address, unsigned width) const;
    // Apply controller/mask/Admin setup or queue doorbells and process synchronous commands.
    void write(uint64_t address, uint64_t value, unsigned width);
    uint64_t requests = 0;
    // Describe host request/error counters and current CC/CSTS without changing guest state.
    std::string diagnostics() const;

  private:
    // IMPLEMENTATION NOTE: Sixteen queue slots include Admin; CAP.MQES limits entries.
    static constexpr unsigned QueueCount = 16, MaximumQueueEntries = 256;
    // Number of I/O SQ/CQ entries in the Number of Queues feature is zero-based:
    // sixteen total slots minus Admin minus the zero-base offset gives 14 (000e000eh).
    static constexpr uint32_t DefaultQueueFeature = ((QueueCount - 2) << 16) | (QueueCount - 2);
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
    std::array<Queue, QueueCount> sq{}, cq{};
    uint64_t asq = 0, acq = 0;
    uint32_t bar = 0, cc = 0, csts = 0, aqa = 0, interruptMask = 0;
    uint16_t pciCommand = 0;
    bool probe = false;
    uint32_t queueFeature = DefaultQueueFeature, asyncConfig = 0, coalescing = 0;
    uint64_t adminRequests = 0, ioRequests = 0, errors = 0, resets = 0;
    uint32_t lastError = 0;
    unsigned asyncPending = 0;
    // Resolve a bounded guest physical RAM span; return empty for invalid DMA.
    std::span<uint8_t> memory(uint64_t address, size_t bytes);
    // Assert INTx for a pending interrupt-enabled CQ unless vector/PCI masking disables it.
    void update_intx();
    // Validate Admin geometry and supported CC format, initialize queues or set fatal status.
    void enable_controller();
    // Consume SQ commands while CQ space exists, publish completion fields/phase and refresh INTx.
    void process_submission_queue(unsigned qid);
    // Execute the supported Admin/NVM command subset and return unshifted completion status.
    uint16_t execute_command(unsigned qid, std::span<const uint8_t> cmd, uint32_t &result,
                             bool &defer);
    // Validate all PRP buffers/list entries before copying a data payload in either direction.
    bool transfer_prp_data(std::span<const uint8_t> cmd, std::span<uint8_t> data, bool toGuest);
};
} // namespace pc
