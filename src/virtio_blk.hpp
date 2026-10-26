#pragma once
#include "disk.hpp"
#include "virtio_registers.hpp"
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
    // Bind the disk, guest RAM and interrupt callback for the single request queue.
    VirtioBlock(Disk &disk, std::span<uint8_t> ram, uint64_t ramBase,
                std::function<void(bool)> interrupt);
    // Read MMIO transport state or a synthesized little-endian block configuration.
    uint64_t read(unsigned offset, unsigned width = 4) const;
    // Apply modern MMIO feature, queue, status and interrupt-acknowledgement writes.
    void write(unsigned offset, uint64_t value, unsigned width = 4);
    uint64_t requests = 0, bytesRead = 0, bytesWritten = 0;
    // Inspect the last host queue failure without changing guest state.
    // IMPLEMENTATION NOTE: Host diagnostic observation around the block protocol.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2850006
    const std::string &error() const {
        return failure;
    }

  private:
    struct Buffer {
        uint64_t address;
        uint32_t length;
        bool writable;
    };
    Disk &disk;
    std::span<uint8_t> ram;
    uint64_t ramBase;
    std::function<void(bool)> interrupt;
    uint32_t featureSelect = 0, driverSelect = 0, queueSelect = 0;
    uint32_t queueSize = 0, ready = 0, status = 0, interruptStatus = 0;
    uint64_t driverFeatures = 0, descriptors = 0, available = 0, used = 0;
    uint16_t lastAvailable = 0, usedIndex = 0;
    std::string failure;
    // Virtio 1.2, 5.2.3 feature positions: size_max, seg_max, blk_size and FLUSH.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2770003
    static constexpr unsigned MaximumSegmentBytesFeatureBit = 1, MaximumSegmentsFeatureBit = 2,
                              LogicalBlockSizeFeatureBit = 6, FlushFeatureBit = 9;
    static constexpr uint64_t features =
        virtio::Version1Feature | (1ull << MaximumSegmentBytesFeatureBit) |
        (1ull << MaximumSegmentsFeatureBit) | (1ull << LogicalBlockSizeFeatureBit) |
        (1ull << FlushFeatureBit);
    // Resolve a guest physical range with overflow-safe bound checks.
    uint8_t *memory(uint64_t address, uint64_t length) const;
    // Read a little-endian ring field from validated guest RAM.
    uint64_t read_guest_value(uint64_t address, unsigned width) const;
    // Publish a little-endian ring field to validated guest RAM.
    void write_guest_value(uint64_t address, uint64_t value, unsigned width);
    // Clear transport/queue/error state and lower interrupt; retain host I/O counters.
    void reset();
    // Validate split-ring geometry and all RAM extents before queue traversal.
    void validate_queue() const;
    // Consume available requests, publish used indices/lengths and unsuppressed interrupts.
    void process_request_queue();
    // Execute IN/OUT/FLUSH/GET_ID, write completion status and return guest-written bytes.
    uint32_t execute_request(uint16_t head);
    // Collect validated direct buffers; reject cycles and readable-after-writable order.
    std::vector<Buffer> descriptor_chain(uint16_t head) const;
    // Copy a slice across readable or writable chain buffers, rejecting a short aggregate span.
    void copy_chain_bytes(const std::vector<Buffer> &buffers, bool writable, size_t skip,
                          std::span<uint8_t> data, bool toGuest);
    // Record a queue failure, set DEVICE_NEEDS_RESET and assert configuration interrupt.
    void require_device_reset(const std::string &reason);
};
} // namespace pc
