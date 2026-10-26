#pragma once
#include "arm_hyperv_ic_protocol.hpp"
#include "arm_vmbus.hpp"
#include <optional>
namespace pc {
// Microsoft OpenVMM TimesyncMessageV4, service 4.0 with framework 1.0/3.0.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54
// Host IC timesync v4. UTC is FILETIME (100 ns since 1601), paired by the
// caller with the guest's monotonic reference time. All VPs must be stopped.
class ArmHyperVTimeSync {
  public:
    // IMPLEMENTATION NOTE: Fixed host channel/event allocation; GUIDs identify service.
    static constexpr uint32_t ChannelId = 2, ConnectionId = 17;
    // IMPLEMENTATION NOTE: Five-second sample cadence, thirty-second reply deadline.
    static constexpr uint64_t SampleInterval100ns = 5 * hyperv_ic::TicksPerSecond100ns,
                              ResponseTimeout100ns = 30 * hyperv_ic::TicksPerSecond100ns;
    // Return the named-pipe time-sync offer with its documented interface/instance GUIDs.
    static ArmVmbus::Offer channel_offer();
    // Convert signed Unix-epoch 100 ns ticks to FILETIME; reject times before 1601.
    static uint64_t unix_100ns_to_filetime(int64_t ticks);
    // Begin version negotiation and reset session transaction/initial-sync state.
    void channel_opened(uint64_t now);
    // Stop the session and clear its selected framework.
    void channel_closed();
    // Rebuild an unpublished sample on retry so ring backpressure cannot make
    // a stale UTC/reference pair appear freshly sampled.
    // Build a due negotiation or fresh paired UTC/reference sample without consuming it.
    std::vector<uint8_t> pending_packet(uint64_t now, uint64_t utcFiletime);
    // Start a reply deadline only after the candidate reaches the ring.
    void packet_published(uint64_t now);
    // Accept only matching version/transaction/status replies before the deadline.
    void receive_response(std::span<const uint8_t> packet, uint64_t now);
    // Return ticks until pending work/deadline, or none when idle/failed.
    std::optional<uint64_t> next_delay_100ns(uint64_t now) const;
    // Report protocol/sample/acknowledgment counters without claiming clock adjustment.
    std::string report() const;

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    enum class State { Closed, SendVersion, WaitVersion, Ready, WaitSample, Failed };
    State state = State::Closed;
    uint16_t framework = 0;
    uint8_t transaction = 0;
    bool initial = true;
    uint64_t since = 0, negotiations = 0, samples = 0, acks = 0, rejected = 0, timeouts = 0;
};
} // namespace pc
