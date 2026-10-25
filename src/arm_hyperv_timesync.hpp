#pragma once
#include "arm_vmbus.hpp"
#include <optional>
namespace pc {
// Host IC timesync v4. UTC is FILETIME (100 ns since 1601), paired by the
// caller with the guest's monotonic reference time. All VPs must be stopped.
class ArmHyperVTimeSync {
  public:
    static constexpr uint32_t channelId=2, connectionId=17;
    static constexpr uint64_t interval=50000000, timeout=300000000;
    static ArmVmbus::Offer offer();
    static uint64_t utcFromUnix100ns(int64_t ticks);
    void opened(uint64_t now);
    void closed();
    // Rebuild an unpublished sample on retry so ring backpressure cannot make
    // a stale UTC/reference pair appear freshly sampled.
    std::vector<uint8_t> packet(uint64_t now,uint64_t utcFiletime);
    void sent(uint64_t now);
    void receive(std::span<const uint8_t> packet,uint64_t now);
    std::optional<uint64_t> delay(uint64_t now) const;
    std::string report() const;
  private:
    enum class State { Closed, SendVersion, WaitVersion, Ready, WaitSample, Failed };
    State state=State::Closed;
    uint16_t framework=0;
    uint8_t transaction=0;
    bool initial=true;
    uint64_t since=0,negotiations=0,samples=0,acks=0,rejected=0,timeouts=0;
};
}
