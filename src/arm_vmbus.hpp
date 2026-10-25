#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace pc {
// VTL0 VMBus control protocol. Channel offers and GPADL wire handling are
// not attached yet; ArmGpadl provides the separate mapping/assembly foundation.
// Invoked only with guest VPs stopped, like ArmHyperV/SynIC.
class ArmVmbus {
  public:
    using Send = std::function<uint16_t(unsigned, unsigned, std::span<const uint8_t>)>;
    ArmVmbus(std::span<const uint8_t> ram, uint64_t base, unsigned cpus);
    void setSender(Send sender) { send = std::move(sender); }
    // Connection 1 is the legacy single-client bus (fixed SINT2).
    // Bootstrap connection 4 opts into the target-info/multiclient ABI.
    uint16_t receive(uint32_t messageType, std::span<const uint8_t> payload, bool multiclient = false);
    void service();
    bool needsService() const { return !replies.empty(); }
    std::string report() const;
  private:
    struct Reply { unsigned vp, sint; std::vector<uint8_t> payload; };
    std::deque<Reply> replies;
    std::span<const uint8_t> ram;
    uint64_t base;
    unsigned cpus, targetVp = 0, targetSint = 2;
    bool connected = false;
    uint32_t version = 0;
    uint64_t contacts = 0, offers = 0, unloads = 0, delivered = 0, rejected = 0;
    Send send;
    bool validPage(uint64_t gpa) const;
    uint16_t reject(uint16_t status);
};
}
