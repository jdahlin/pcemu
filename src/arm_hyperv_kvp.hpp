#pragma once
#include "arm_hyperv_kvp_codec.hpp"
#include "arm_vmbus.hpp"
namespace pc {
// Host KVP v4 IC. Negotiation is automatic; guest queries are explicit.
// One outstanding enumeration and one unread result, no mutations or retries.
class ArmHyperVKvp {
  public:
    static constexpr uint32_t channelId=4,connectionId=19;
    static constexpr uint64_t timeout=30*10000000ull;
    struct Result {
        ArmHyperVKvpCodec::Pool pool;
        uint32_t index,status;
        std::optional<ArmHyperVKvpCodec::Record> record;
    };
    static ArmVmbus::Offer offer();
    void opened(uint64_t now);
    void closed();
    bool enumerate(ArmHyperVKvpCodec::Pool pool,uint32_t index);
    std::optional<Result> takeResult();
    std::vector<uint8_t> packet(uint64_t now);
    void sent(uint64_t now);
    void receive(std::span<const uint8_t> packet,uint64_t now);
    std::optional<uint64_t> delay(uint64_t now) const;
    std::string report() const;
  private:
    enum class State {Closed,SendVersion,WaitVersion,Ready,SendQuery,WaitResponse,Failed};
    State state=State::Closed;
    uint16_t framework=0;
    uint8_t transaction=0;
    ArmHyperVKvpCodec::Pool pool=ArmHyperVKvpCodec::Pool::Auto;
    uint32_t index=0,lastStatus=0;
    uint64_t since=0,negotiations=0,requests=0,responses=0,records=0,ends=0,rejected=0,timeouts=0;
    std::optional<Result> result;
    void expire(uint64_t now);
};
}
