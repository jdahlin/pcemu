#pragma once
#include "virtio_registers.hpp"
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
    // Bind RAM and IRQ; use host cryptographic entropy unless a source is supplied.
    VirtioRng(std::span<uint8_t> ram, uint64_t ramBase, std::function<void(bool)> interrupt,
              Entropy entropy = {});
    // Read a modern MMIO register or the empty device configuration window.
    uint64_t read(unsigned offset, unsigned width = 4) const;
    // Write modern MMIO state or notify the sole entropy queue.
    void write(unsigned offset, uint64_t value, unsigned width = 4);
    // Inspect the last host-visible queue failure without changing guest state.
    // IMPLEMENTATION NOTE: Diagnostic access around the entropy transport.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3110006
    const std::string &error() const {
        return failure;
    }
    uint64_t bytesSupplied = 0;

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    std::span<uint8_t> ram;
    uint64_t ramBase;
    std::function<void(bool)> interrupt;
    Entropy entropy;
    uint32_t featureSelect = 0, driverSelect = 0, queueSelect = 0;
    uint32_t queueSize = 0, ready = 0, status = 0, interruptStatus = 0;
    uint64_t driverFeatures = 0, descriptors = 0, available = 0, used = 0;
    uint16_t lastAvailable = 0, usedIndex = 0;
    std::string failure;
    // Only the modern transport feature is offered; entropy has no device feature bits.
    static constexpr uint64_t features = virtio::Version1Feature;
    // Return a validated guest physical RAM pointer.
    uint8_t *memory(uint64_t address, uint64_t length) const;
    // Read one little-endian guest field.
    uint64_t read_guest_value(uint64_t address, unsigned width) const;
    // Write one little-endian guest field.
    void write_guest_value(uint64_t address, uint64_t value, unsigned width);
    // Clear transport/queue state and lower the interrupt.
    void reset();
    // Check split-ring alignment, size and RAM extents.
    void validate_queue() const;
    // Consume available requests and publish used bytes/completion interrupt.
    void process_request_queue();
    // Fill a validated writable direct chain up to the per-request limit.
    uint32_t fill_entropy_request(uint16_t head);
    // Set DEVICE_NEEDS_RESET and record/notify the processing error.
    void require_device_reset(const std::string &reason);
};
} // namespace pc
