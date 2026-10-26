#pragma once
#include "virtio_device.hpp"
#include <deque>
namespace pc {
// Named generic ports with independent buffers and the multiport handshake.
class VirtioSerial : public VirtioDevice {
  public:
    VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                 std::string name = "org.qemu.guest_agent.0");
    VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                 std::vector<std::string> names);
    void hostConnected(bool connected, unsigned port = 0);
    size_t receive(std::span<const uint8_t> bytes, unsigned port = 0);
    std::vector<uint8_t> takeOutput(size_t maximum, unsigned port = 0);
    size_t receiveCapacity(unsigned port = 0) const;
    void poll();
    bool portReady(unsigned port = 0) const {
        return ports.at(port).ready;
    }
    bool portOpen(unsigned port = 0) const {
        return ports.at(port).guestOpen;
    }
    uint64_t received = 0, transmitted = 0;

  private:
    static constexpr size_t limit = 1024 * 1024;
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
    void notified(unsigned) override {
        poll();
    }
    void control(uint16_t event, uint16_t value, const std::string &text = {}, unsigned port = 0);
};
} // namespace pc
