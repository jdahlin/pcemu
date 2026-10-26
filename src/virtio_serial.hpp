#pragma once
#include "virtio_device.hpp"
#include <deque>
namespace pc {
// One named generic port, with the standard multiport control handshake.
class VirtioSerial : public VirtioDevice {
  public:
    VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                 std::string name = "org.qemu.guest_agent.0");
    void hostConnected(bool connected);
    size_t receive(std::span<const uint8_t> bytes);
    std::vector<uint8_t> takeOutput(size_t maximum);
    size_t receiveCapacity() const;
    void poll();
    bool portReady() const {
        return ready;
    }
    bool portOpen() const {
        return guestOpen;
    }
    uint64_t received = 0, transmitted = 0;

  private:
    static constexpr size_t limit = 1024 * 1024;
    std::string name;
    bool hostOpen = false, guestOpen = false, deviceReady = false, ready = false;
    std::deque<std::vector<uint8_t>> controls;
    std::deque<uint8_t> incoming, outgoing;
    std::optional<uint32_t> handle(unsigned queue, const Chain &chain) override;
    uint64_t configRead(unsigned offset, unsigned width) const override;
    void deviceReset() override;
    void notified(unsigned) override {
        poll();
    }
    void control(uint16_t event, uint16_t value, const std::string &text = {});
};
} // namespace pc
