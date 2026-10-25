#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <utility>
namespace pc {
// VTL0 ARM SynIC. All calls require stopped vCPUs, including RAM updates.
// The caller supplies real GIC delivery; this class never fabricates an IRQ.
class ArmSynic {
  public:
    using Interrupt = std::function<bool(uint64_t, unsigned)>;
    ArmSynic(std::span<uint8_t> ram, uint64_t base, unsigned cpus);
    void setInterruptSender(Interrupt send) { interrupt = std::move(send); }
    // Rescan after a stopped execution slice: GIC EOI is handled natively
    // and need not cause a host exit. Occupied slots are never overwritten.
    uint16_t service();
    bool needsService() const;
    // Bounded metadata only; never exposes arbitrary message payloads.
    std::string report() const;
    uint16_t read(uint32_t name, unsigned vp, uint64_t &value) const;
    uint16_t write(uint32_t name, unsigned vp, uint64_t value);
    uint16_t signalEvent(unsigned vp, unsigned sint, unsigned flag, bool &newlySignaled);
    // Dedicated timer buffers remain owned by the timer until a slot is free.
    uint16_t tryPostMessage(unsigned vp, unsigned sint, uint32_t type,
                            uint64_t sender, std::span<const uint8_t> payload);
    uint16_t postMessage(unsigned vp, unsigned sint, uint32_t type,
                         uint64_t sender, std::span<const uint8_t> payload);
  private:
    using Message = std::array<uint8_t, 256>;
    struct Vp {
        uint64_t control = 0, events = 0, messages = 0;
        std::array<uint64_t, 16> sint;
        std::array<std::deque<Message>, 16> queue;
        Vp() { sint.fill(1ull << 16); }
    };
    std::array<Vp, 8> state;
    std::span<uint8_t> ram;
    uint64_t base;
    unsigned cpus;
    Interrupt interrupt;
    std::span<uint8_t> page(uint64_t reg) const;
    bool notify(unsigned vp, unsigned sint);
    uint16_t drain(unsigned vp, unsigned sint);
};
}
