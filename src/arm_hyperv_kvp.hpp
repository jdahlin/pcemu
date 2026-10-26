#pragma once
#include "arm_hyperv_ic_protocol.hpp"
#include "arm_hyperv_kvp_codec.hpp"
#include "arm_vmbus.hpp"
namespace pc {
// Host Key-Value Pair (KVP) Integration Component (IC), service 3.0/4.0/5.0.
// Microsoft OpenVMM standard KvpMessage and IC Header/NegotiateMessage:
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
// Host KVP v3/v4/v5 standard-payload IC. Negotiation is automatic; guest queries are explicit.
// One outstanding explicit operation and one unread result; no automatic retries.
class ArmHyperVKvp {
  public:
    // IMPLEMENTATION NOTE: Fixed host channel/event allocation, separate from service GUIDs.
    static constexpr uint32_t ChannelId = 4, ConnectionId = 19;
    // IMPLEMENTATION NOTE: Thirty-second host response deadline in 100 ns ticks.
    static constexpr uint64_t ResponseTimeout100ns = 30 * hyperv_ic::TicksPerSecond100ns;
    struct Result {
        ArmHyperVKvpCodec::Pool pool;
        uint32_t index, status;
        std::optional<ArmHyperVKvpCodec::Record> record;
        ArmHyperVKvpCodec::Operation operation = ArmHyperVKvpCodec::Operation::Enumerate;
    };
    // Return the KVP named-pipe offer with its documented interface/instance GUIDs.
    static ArmVmbus::Offer channel_offer();
    // Start negotiation and clear the previous result/query/session diagnostics.
    void channel_opened(uint64_t now);
    // Stop protocol work and discard any pending query/unread result.
    void channel_closed();
    // Queue one explicit pool/index enumeration when ready and no unread result remains.
    bool enumerate_value(ArmHyperVKvpCodec::Pool pool, uint32_t index);
    // Queue one explicit nonempty-key GET when ready and no unread result remains.
    bool get_value(ArmHyperVKvpCodec::Pool pool, const std::u16string &key);
    // Queue an explicit typed SET only in host-writable External/AutoExternal pools.
    bool set_value(ArmHyperVKvpCodec::Pool pool, const std::u16string &key,
                   const ArmHyperVKvpCodec::Value &value);
    // Queue an explicit DELETE only in host-writable External/AutoExternal pools.
    bool delete_value(ArmHyperVKvpCodec::Pool pool, const std::u16string &key);
    // Move the unread result to the caller, permitting the next explicit query.
    std::optional<Result> take_result();
    // Build the due negotiation/query candidate without starting a response deadline.
    std::vector<uint8_t> pending_packet(uint64_t now);
    // Count/start a response deadline only after actual ring publication.
    void packet_published(uint64_t now);
    // Validate versions/transaction/status and retain one decoded result for the host.
    void receive_response(std::span<const uint8_t> packet, uint64_t now);
    // Return ticks until work/deadline, or none when idle/failed.
    std::optional<uint64_t> next_delay_100ns(uint64_t now) const;
    // Report bounded state/version/status metadata, excluding guest keys and values.
    std::string report() const;

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    enum class State { Closed, SendVersion, WaitVersion, Ready, SendQuery, WaitResponse, Failed };
    State state = State::Closed;
    uint16_t framework = 0, service = 0;
    uint8_t transaction = 0;
    ArmHyperVKvpCodec::Pool pool = ArmHyperVKvpCodec::Pool::Auto;
    uint32_t index = 0, lastStatus = 0;
    uint32_t versionReserved = 0;
    uint16_t versionBodySize = 0, versionFrameworkCount = 0, versionServiceCount = 0;
    uint32_t versionFramework = 0, versionService = 0;
    uint64_t since = 0, negotiations = 0, requests = 0, responses = 0, records = 0, ends = 0,
             rejected = 0, timeouts = 0;
    ArmHyperVKvpCodec::Operation operation = ArmHyperVKvpCodec::Operation::Enumerate;
    std::vector<uint8_t> queryBody;
    std::u16string queryKey;
    uint64_t mutationAcks = 0;
    // Validate state/pool/operation and encode the pending query before changing session state.
    bool queue_request(ArmHyperVKvpCodec::Operation operation, ArmHyperVKvpCodec::Pool pool,
                       uint32_t index, const std::u16string &key = {},
                       const ArmHyperVKvpCodec::Value *value = nullptr);
    std::optional<Result> result;
    // Fail an outstanding negotiation/query after its host response deadline.
    void expire_response(uint64_t now);
};
} // namespace pc
