#pragma once
#include "arm_hyperv_ic_protocol.hpp"
#include "arm_vmbus.hpp"
#include <optional>
namespace pc {
// Host heartbeat Integration Component (IC), framework/service versions 1.0/3.0.
// Microsoft OpenVMM IC header/HeartbeatMessage plus Linux legacy heartbeat body.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/heartbeat.rs
// All guest virtual processors must be stopped during service/state transitions.
class ArmHyperVHeartbeat {
  public:
    // IMPLEMENTATION NOTE: Fixed host channel/event allocation, not service identity GUIDs.
    static constexpr uint32_t ChannelId = 1, ConnectionId = 16;
    // IMPLEMENTATION NOTE: Probe five seconds after success; retire/retry at thirty seconds.
    static constexpr uint64_t ProbeInterval100ns = 5 * hyperv_ic::TicksPerSecond100ns,
                              ResponseTimeout100ns = 30 * hyperv_ic::TicksPerSecond100ns;
    // Return the heartbeat named-pipe offer with distinct interface/instance GUIDs.
    static ArmVmbus::Offer channel_offer();
    // Begin negotiation and reset liveness flags while preserving host counters/sequence.
    void channel_opened(uint64_t now);
    // Disable session activity and clear liveness/version state.
    void channel_closed();
    // A candidate is retained logically until packet_published() confirms ring publication.
    // Build the due negotiation/probe without consuming publication ownership.
    std::vector<uint8_t> pending_packet(uint64_t now);
    // Advance a successfully published candidate to its response-wait state/deadline.
    void packet_published(uint64_t now);
    // Validate an IC response and accept only the negotiated version/current sequence.
    void receive_response(std::span<const uint8_t> packet, uint64_t now);
    // Return the remaining 100 ns ticks to service, zero for immediate work, or none when idle.
    std::optional<uint64_t> next_delay_100ns(uint64_t now) const;
    // Report protocol state, transport liveness and guest application state separately.
    std::string report() const;

  private:
    enum class State { Closed, SendVersion, WaitVersion, Ready, WaitHeartbeat, Failed };
    State state = State::Closed;
    bool responsive = false, expired = false, unresponsive = false;
    uint16_t framework = 0, version = 0;
    uint32_t applicationState = 0;
    uint64_t since = 0, sequence = 0, probes = 0, responses = 0, rejected = 0, timeouts = 0,
             negotiations = 0;
};
} // namespace pc
