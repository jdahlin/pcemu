#pragma once
#include "arm_vmbus.hpp"
#include <optional>
namespace pc {
// Host shutdown IC v3.2. Negotiation never requests a power operation.
class ArmHyperVShutdown {
  public:
    enum class Action { PowerOff, Reboot, Hibernate };
    static constexpr uint32_t channelId=3,connectionId=18;
    static constexpr uint64_t responseTimeout=300000000;
    static ArmVmbus::Offer offer();
    void opened(uint64_t now);
    void closed();
    bool request(Action action,bool force=false,uint32_t timeoutSeconds=0);
    std::vector<uint8_t> packet(uint64_t now);
    void sent(uint64_t now);
    void receive(std::span<const uint8_t> packet,uint64_t now);
    std::optional<uint64_t> delay(uint64_t now) const;
    std::string report() const;
  private:
    enum class State { Closed,SendVersion,WaitVersion,Ready,SendRequest,WaitResponse,Accepted,Failed };
    State state=State::Closed;
    uint16_t framework=0;
    uint8_t transaction=0;
    uint32_t flags=0,guestTimeout=0,lastStatus=0;
    uint64_t since=0,negotiations=0,requests=0,accepted=0,denied=0,rejected=0,timeouts=0;
};
}
