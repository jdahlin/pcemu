#pragma once
#include "virtio_registers.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pc {
// Modern split-ring core for the guest-tool devices. All calls run with VPs stopped.
class VirtioDevice {
  public:
    using Interrupt = std::function<void(bool, unsigned)>;
    // Bind guest RAM, device type/features, queues and interrupt delivery callback.
    VirtioDevice(std::span<uint8_t> ram, uint64_t base, uint32_t id, unsigned queues,
                 uint64_t features, Interrupt interrupt);
    // Release host state; guest-visible reset is a separate operation.
    // IMPLEMENTATION NOTE: Host lifecycle/dispatch around Virtio 1.2 transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
    virtual ~VirtioDevice() = default;
    // Read a transport register or supported device configuration bytes.
    uint64_t read(unsigned offset, unsigned width = 4) const;
    // Apply a transport write or forward supported device configuration bytes.
    void write(unsigned offset, uint64_t value, unsigned width = 4);
    // Expose the last host processing error; no guest state is changed.
    // IMPLEMENTATION NOTE: Host lifecycle/dispatch around Virtio 1.2 transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
    const std::string &error() const {
        return failure;
    }
    // Report successful initialization with neither driver nor device failure set.
    // IMPLEMENTATION NOTE: Host lifecycle/dispatch around Virtio 1.2 transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
    bool running() const {
        return (status & InitializedStatus) == InitializedStatus &&
               !(status & (virtio::DeviceNeedsReset | virtio::Failed));
    }
    // Return the last queue to assert a used-buffer interrupt.
    // IMPLEMENTATION NOTE: Host lifecycle/dispatch around Virtio 1.2 transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
    unsigned interruptQueue() const {
        return lastInterruptQueue;
    }
    uint64_t completions = 0, resetCount = 0;

  protected:
    struct Segment {
        std::span<uint8_t> bytes;
        bool writable;
    };
    struct Chain {
        std::vector<Segment> segments;
        size_t readable = 0, writable = 0;
        // Gather readable segments, rejecting requests larger than maximum.
        std::vector<uint8_t> input(size_t maximum) const;
        // Scatter response bytes to writable segments; throw if they do not fit.
        void output(std::span<const uint8_t> data) const;
    };
    // Handle a chain: used length completes it; no result defers it without consumption.
    virtual std::optional<uint32_t> handle(unsigned queue, const Chain &chain) = 0;
    // Default device configuration reads zero; derived devices override supported fields.
    // IMPLEMENTATION NOTE: Host lifecycle/dispatch around Virtio 1.2 transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
    virtual uint64_t read_device_configuration(unsigned, unsigned) const {
        return 0;
    }
    // Default device configuration writes have no effect.
    // IMPLEMENTATION NOTE: Host lifecycle/dispatch around Virtio 1.2 transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
    virtual void write_device_configuration(unsigned, uint64_t, unsigned) {}
    // Default reset hook has no device-specific state to clear.
    // IMPLEMENTATION NOTE: Host lifecycle/dispatch around Virtio 1.2 transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
    virtual void reset_device() {}
    // Default notification hook processes the selected queue synchronously.
    // IMPLEMENTATION NOTE: Host lifecycle/dispatch around Virtio 1.2 transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
    virtual void notified(unsigned queue) {
        process_queue(queue);
    }
    // Consume available buffers, publish completions and signal the used interrupt.
    void process_queue(unsigned queue);
    // Increment configuration generation and signal the configuration interrupt.
    void configuration_changed();
    // Test a feature bit accepted by the driver.
    // IMPLEMENTATION NOTE: Host lifecycle/dispatch around Virtio 1.2 transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
    bool negotiated(unsigned bit) const {
        return (driverFeatures & (1ull << bit)) != 0;
    }
    // Resolve a bounded guest physical RAM span; throw outside bound RAM.
    std::span<uint8_t> memory(uint64_t address, uint64_t length) const;
    // Record a host error, mark DEVICE_NEEDS_RESET and notify the driver.
    void require_device_reset(const std::string &reason);

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    // All four initialization flags are required before processing buffers (2.1).
    static constexpr uint32_t InitializedStatus =
        virtio::Acknowledge | virtio::Driver | virtio::FeaturesOk | virtio::DriverOk;
    // IMPLEMENTATION NOTE: Bound queue storage and descriptor cycle detection.
    static constexpr unsigned MaximumQueueEntries = 256;
    struct Queue {
        // IMPLEMENTATION NOTE: Default and maximum queue capacity for this core.
        uint32_t size = MaximumQueueEntries;
        bool ready = false;
        uint64_t descriptors = 0, available = 0, used = 0;
        uint16_t nextAvailable = 0, nextUsed = 0;
    };
    std::span<uint8_t> ram;
    uint64_t ramBase, features;
    uint32_t id, featureSelect = 0, driverSelect = 0, queueSelect = 0;
    uint32_t status = 0, interruptStatus = 0, generation = 0;
    uint64_t driverFeatures = 0;
    unsigned lastInterruptQueue = 0;
    std::vector<Queue> queues;
    Interrupt interrupt;
    std::string failure;
    // Read a little-endian field from guest RAM.
    uint64_t read_guest_value(uint64_t address, unsigned width) const;
    // Write a little-endian field in guest RAM.
    void write_guest_value(uint64_t address, uint64_t value, unsigned width);
    // Check split-ring geometry and all ring RAM extents.
    void validate_queue(const Queue &queue) const;
    // Walk validated direct descriptors and enforce readable-before-writable order.
    Chain descriptor_chain(const Queue &queue, uint16_t head) const;
    // Clear negotiated state/queues, invoke the device hook and lower interrupt.
    void reset();
};
} // namespace pc
