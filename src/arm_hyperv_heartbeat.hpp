#pragma once
#include "arm_vmbus.hpp"
#include <optional>
namespace pc {
// Host side of the heartbeat integration component. All VPs must be stopped.
class ArmHyperVHeartbeat {
  public:
    static constexpr uint32_t channelId = 1, connectionId = 16;
    static constexpr uint64_t interval = 5 * 10000000ull, timeout = 30 * 10000000ull;
    static ArmVmbus::Offer offer();
    void opened(uint64_t now);
    void closed();
    // A candidate is retained logically until sent() confirms ring publication.
    std::vector<uint8_t> packet(uint64_t now);
    void sent(uint64_t now);
    void receive(std::span<const uint8_t> packet, uint64_t now);
    std::optional<uint64_t> delay(uint64_t now) const;
    std::string report() const;
  private:
    enum class State { Closed, SendVersion, WaitVersion, Ready, WaitHeartbeat, Failed };
    State state = State::Closed;
    bool responsive = false, expired = false, unresponsive = false;
    uint16_t framework = 0, version = 0;
    uint32_t applicationState = 0;
    uint64_t since = 0, sequence = 0, probes = 0, responses = 0, rejected = 0, timeouts = 0, negotiations = 0;
};
}
