#pragma once
#include "virtio_device.hpp"
#include <deque>
namespace pc {
// Named generic ports with independent buffers and the multiport handshake.
class VirtioSerial : public VirtioDevice {
  public:
    // Create one or multiple fixed named generic ports.
    VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                 std::string name = "org.qemu.guest_agent.0");
    // Create one or multiple fixed named generic ports.
    VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                 std::vector<std::string> names);
    // Set host open state and discard stale bytes on a connection transition.
    void set_host_port_open(bool connected, unsigned port = 0);
    // Queue host input up to available capacity; return bytes accepted.
    size_t receive_from_host(std::span<const uint8_t> bytes, unsigned port = 0);
    // Remove up to maximum bytes of guest output for the host.
    std::vector<uint8_t> take_guest_output(size_t maximum, unsigned port = 0);
    // Return input capacity while both ends are open and the port is ready.
    size_t host_receive_capacity(unsigned port = 0) const;
    // Progress handshakes and any data queues able to make progress.
    void poll();
    // Report whether the driver completed PORT_READY.
    // IMPLEMENTATION NOTE: Host observation/dispatch of Virtio console state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3000002
    bool port_ready(unsigned port = 0) const {
        return ports.at(port).ready;
    }
    // Report the driver’s PORT_OPEN state.
    // IMPLEMENTATION NOTE: Host observation/dispatch of Virtio console state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3000002
    bool guest_port_open(unsigned port = 0) const {
        return ports.at(port).guestOpen;
    }
    uint64_t received = 0, transmitted = 0;

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    // IMPLEMENTATION NOTE: Bound each incoming/outgoing port backlog to 1 MiB.
    static constexpr size_t PortBufferBytes = 1024 * 1024;
    struct Port {
        std::string name;
        bool hostOpen = false, guestOpen = false, ready = false;
        std::deque<uint8_t> incoming, outgoing;
    };
    std::vector<Port> ports;
    bool deviceReady = false;
    std::deque<std::vector<uint8_t>> controls;
    std::optional<uint32_t> handle(unsigned queue, const Chain &chain) override;
    uint64_t read_device_configuration(unsigned offset, unsigned width) const override;
    void reset_device() override;
    // Service all queues so a control handshake can unblock waiting data queues.
    // IMPLEMENTATION NOTE: Host observation/dispatch of Virtio console state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3000002
    void notified(unsigned) override {
        // Progress handshakes and any data queues able to make progress.
        poll();
    }
    void queue_control_message(uint16_t event, uint16_t value, const std::string &text = {},
                               unsigned port = 0);
};
} // namespace pc
