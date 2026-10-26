#pragma once
#include "virtio_registers.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace pc {
// Modern virtio-net, one RX/TX split-queue pair. All DMA runs with the vCPU stopped.
class VirtioNet {
  public:
    using Bytes = std::vector<uint8_t>;
    // Bind guest RAM and interrupt delivery for one receive/transmit queue pair.
    VirtioNet(std::span<uint8_t> ram, uint64_t ramBase, std::function<void(bool)> interrupt);
    // Read MMIO state or the fixed MAC/link/MTU device configuration.
    uint64_t read(unsigned offset, unsigned width = 4) const;
    // Apply feature/status/queue writes, acknowledge interrupt causes or process transmit
    // notification.
    void write(unsigned offset, uint64_t value, unsigned width = 4);
    // Consume one posted receive chain for an incoming frame or count a drop; notify completion.
    void receive(const uint8_t *data, size_t length);
    std::deque<Bytes> outgoing;
    // Inspect the last queue that asserted a used-buffer notification.
    // IMPLEMENTATION NOTE: Host routing observation around Virtio split queues.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
    unsigned interruptQueue() const {
        return signaledQueue;
    }
    uint64_t transmitted = 0, received = 0, dropped = 0, resetCount = 0;
    // Inspect the last host-visible queue failure.
    // IMPLEMENTATION NOTE: Diagnostic observation around the network device.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
    const std::string &error() const {
        return failure;
    }

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    // IMPLEMENTATION NOTE: Bound per-queue state and descriptor cycle tracking.
    static constexpr unsigned MaximumQueueEntries = 256;
    struct Queue {
        uint32_t size = MaximumQueueEntries, ready = 0;
        uint64_t descriptors = 0, available = 0, used = 0;
        uint16_t last = 0, index = 0;
    };
    struct Buffer {
        uint64_t address;
        uint32_t length;
    };
    std::span<uint8_t> ram;
    uint64_t ramBase;
    std::function<void(bool)> interrupt;
    std::array<Queue, 2> queues{};
    uint32_t featureSelect = 0, driverSelect = 0, queueSelect = 0, status = 0, isr = 0;
    uint64_t driverFeatures = 0;
    unsigned signaledQueue = 0;
    std::string failure;
    // Virtio 1.2, 5.1.3 and section 6: offered feature positions.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
    static constexpr unsigned ChecksumFeatureBit = 0, MtuFeatureBit = 3, MacAddressFeatureBit = 5,
                              LinkStatusFeatureBit = 16, EventIndexFeatureBit = 29;
    static constexpr uint64_t features = virtio::Version1Feature | (1ull << ChecksumFeatureBit) |
                                         (1ull << MtuFeatureBit) | (1ull << MacAddressFeatureBit) |
                                         (1ull << LinkStatusFeatureBit) |
                                         (1ull << EventIndexFeatureBit);
    // Resolve a bounded guest physical RAM range; reject overflow/outside accesses.
    uint8_t *memory(uint64_t address, uint64_t length) const;
    // Read one little-endian ring field from validated RAM.
    uint64_t read_guest_value(uint64_t address, unsigned width) const;
    // Write one little-endian ring field in validated RAM.
    void write_guest_value(uint64_t address, uint64_t value, unsigned width);
    // Check direct split-ring geometry and all RAM extents.
    void validate_queue(const Queue &queue) const;
    // Validate a bounded direct chain whose buffers all have the required direction.
    std::vector<Buffer> descriptor_chain(const Queue &queue, uint16_t head, bool writable) const;
    // Gather validated readable buffers into one packet including its Virtio header.
    Bytes gather_readable_buffers(const std::vector<Buffer> &buffers) const;
    // Copy a complete receive packet across validated writable buffers or reject short capacity.
    void scatter_received_frame(const std::vector<Buffer> &buffers, const Bytes &packet);
    // Return outstanding available entries using wrapping indices; reject ring overrun.
    uint16_t available_count(const Queue &queue) const;
    // Publish used id/length/index and, with EVENT_IDX, request the next available index.
    void publish_used_buffer(Queue &queue, uint16_t head, uint32_t length);
    // Raise used-buffer interrupt only when published entries cross the driver notification
    // threshold.
    void signal_used_buffers(Queue &queue, uint16_t previous);
    // Validate transmit frames, complete optional checksum and queue/drop host frames; publish
    // completions.
    void process_transmit_queue();
    // Record a host queue failure, set DEVICE_NEEDS_RESET and notify configuration change.
    void require_device_reset(const std::string &reason);
};
} // namespace pc
