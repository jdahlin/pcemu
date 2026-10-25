#pragma once
#include "arm_hyperv_kvp_codec.hpp"
#include "arm_vmbus.hpp"
namespace pc {
// Host KVP v3/v4/v5 standard-payload IC. Negotiation is automatic; guest queries are explicit.
// One outstanding explicit operation and one unread result; no automatic retries.
class ArmHyperVKvp {
  public:
    static constexpr uint32_t channelId=4,connectionId=19;
    static constexpr uint64_t timeout=30*10000000ull;
    struct Result {
        ArmHyperVKvpCodec::Pool pool;
        uint32_t index,status;
        std::optional<ArmHyperVKvpCodec::Record> record;
        ArmHyperVKvpCodec::Operation operation=ArmHyperVKvpCodec::Operation::Enumerate;
    };
    static ArmVmbus::Offer offer();
    void opened(uint64_t now);
    void closed();
    bool enumerate(ArmHyperVKvpCodec::Pool pool,uint32_t index);
    bool get(ArmHyperVKvpCodec::Pool pool,const std::u16string &key);
    bool set(ArmHyperVKvpCodec::Pool pool,const std::u16string &key,const ArmHyperVKvpCodec::Value &value);
    bool erase(ArmHyperVKvpCodec::Pool pool,const std::u16string &key);
    std::optional<Result> takeResult();
    std::vector<uint8_t> packet(uint64_t now);
    void sent(uint64_t now);
    void receive(std::span<const uint8_t> packet,uint64_t now);
    std::optional<uint64_t> delay(uint64_t now) const;
    std::string report() const;
  private:
    enum class State {Closed,SendVersion,WaitVersion,Ready,SendQuery,WaitResponse,Failed};
    State state=State::Closed;
    uint16_t framework=0,service=0;
    uint8_t transaction=0;
    ArmHyperVKvpCodec::Pool pool=ArmHyperVKvpCodec::Pool::Auto;
    uint32_t index=0,lastStatus=0;
    uint32_t versionReserved=0;
    uint16_t versionBodySize=0,versionFrameworkCount=0,versionServiceCount=0;
    uint32_t versionFramework=0,versionService=0;
    uint64_t since=0,negotiations=0,requests=0,responses=0,records=0,ends=0,rejected=0,timeouts=0;
    ArmHyperVKvpCodec::Operation operation=ArmHyperVKvpCodec::Operation::Enumerate;
    std::vector<uint8_t> queryBody;
    std::u16string queryKey;
    uint64_t mutationAcks=0;
    bool request(ArmHyperVKvpCodec::Operation operation,ArmHyperVKvpCodec::Pool pool,uint32_t index,
                 const std::u16string &key={},const ArmHyperVKvpCodec::Value *value=nullptr);
    std::optional<Result> result;
    void expire(uint64_t now);
};
}
