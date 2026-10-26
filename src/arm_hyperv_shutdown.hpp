#pragma once
#include "arm_hyperv_ic_protocol.hpp"
#include "arm_vmbus.hpp"
#include <optional>
namespace pc {
// Host shutdown Integration Component (IC), service 3.2, framework 1.0/3.0.
// Microsoft OpenVMM ShutdownMessage / ShutdownFlags / planned reason code.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L34
// Negotiation never requests a power operation. All guest VPs must be stopped.
class ArmHyperVShutdown {
  public:
    enum class Action { PowerOff, Reboot, Hibernate };
    // IMPLEMENTATION NOTE: Fixed host channel/event allocation; service GUIDs are separate.
    static constexpr uint32_t ChannelId = 3, ConnectionId = 18;
    // IMPLEMENTATION NOTE: Host reply deadline, distinct from the guest timeout in seconds.
    static constexpr uint64_t ResponseTimeout100ns = 30 * hyperv_ic::TicksPerSecond100ns;
    // Return the shutdown named-pipe offer with its documented service/instance GUIDs.
    static ArmVmbus::Offer channel_offer();
    // Begin negotiation, clearing prior request/version/transaction status.
    void channel_opened(uint64_t now);
    // Stop session work and clear request parameters/framework.
    void channel_closed();
    // Queue one explicit power-off/reboot/hibernate request only after negotiation is ready.
    bool request_shutdown(Action action, bool force = false, uint32_t timeoutSeconds = 0);
    // Build a negotiation or explicit request; expire missing replies without automatic retry.
    std::vector<uint8_t> pending_packet(uint64_t now);
    // Count/start a reply deadline only after ring publication.
    void packet_published(uint64_t now);
    // Validate transaction/version and record guest acceptance/denial; do not power off the host.
    void receive_response(std::span<const uint8_t> packet, uint64_t now);
    // Return 100 ns ticks until work/deadline, or none when idle/accepted/failed.
    std::optional<uint64_t> next_delay_100ns(uint64_t now) const;
    // Report request flags, guest timeout seconds and guest status separately from host deadlines.
    std::string report() const;

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    enum class State {
        Closed,
        SendVersion,
        WaitVersion,
        Ready,
        SendRequest,
        WaitResponse,
        Accepted,
        Failed
    };
    State state = State::Closed;
    uint16_t framework = 0;
    uint8_t transaction = 0;
    uint32_t flags = 0, guestTimeout = 0, lastStatus = 0;
    uint64_t since = 0, negotiations = 0, requests = 0, accepted = 0, denied = 0, rejected = 0,
             timeouts = 0;
};
} // namespace pc
