#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
namespace pc {
// Four synthetic timers per VP, in partition reference-counter (100 ns) units.
// All service and register operations run with every guest VP stopped.
class ArmHyperVTimers {
  public:
    using Deliver = std::function<bool(unsigned vp, unsigned timer, uint64_t config,
                                      uint64_t expiration, uint64_t delivery)>;
    explicit ArmHyperVTimers(unsigned cpus);
    void setDelivery(Deliver deliver) { delivery = std::move(deliver); }
    uint16_t read(uint32_t name, unsigned vp, uint64_t &value) const;
    uint16_t write(uint32_t name, unsigned vp, uint64_t value, uint64_t now);
    void service(uint64_t now);
    std::optional<uint64_t> nextDeadline(bool includeLazy = true) const;
    std::optional<uint64_t> nextDelay(uint64_t now, bool includeLazy = true) const;
    uint64_t delivered() const { return deliveries; }
  private:
    using Time = __uint128_t;
    struct Timer { uint64_t config = 0, count = 0; Time deadline = 0; bool armed = false; };
    Time lastSample = 0;
    bool sampled = false;
    Time project(uint64_t now) const;
    Time observe(uint64_t now);
    std::optional<Time> deadline(bool includeLazy) const;
    std::array<std::array<Timer, 4>, 8> timers;
    unsigned cpus;
    Deliver delivery;
    uint64_t deliveries = 0;
    static void arm(Timer &timer, Time now);
};
}
