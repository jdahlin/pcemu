#pragma once
#include "arm_vmbus_ring.hpp"
#include <array>
#include <map>
#include <memory>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace pc {
// VTL0 VMBus control and registered-channel transport. No default fake devices.
// Invoked only with guest VPs stopped, like ArmHyperV/SynIC.
class ArmVmbus {
  public:
    using Send = std::function<uint16_t(unsigned, unsigned, std::span<const uint8_t>)>;
    ArmVmbus(std::span<uint8_t> ram, uint64_t base, unsigned cpus);
    void setSender(Send sender) { send = std::move(sender); }
    // Connection 1 is the legacy single-client bus (fixed SINT2).
    // Bootstrap connection 4 opts into the target-info/multiclient ABI.
    uint16_t receive(uint32_t messageType, std::span<const uint8_t> payload, bool multiclient = false);
    struct Offer {
        uint32_t channel, connection;
        std::array<uint8_t, 16> interfaceId, instanceId;
        uint16_t flags = 0;
        std::array<uint8_t, 120> userData{};
    };
    // Returns a complete wire packet, or an empty vector for no response.
    // Trusted backends must not reenter the bus or change its channel inventory.
    using PacketHandler = std::function<std::vector<uint8_t>(std::span<const uint8_t>)>;
    using Event = std::function<uint16_t(unsigned, unsigned, unsigned)>;
    void setEventSender(Event sender) { event = std::move(sender); }
    // Trusted host configuration, before contact. Max eight real backends.
    bool addChannel(const Offer &offer, PacketHandler handler, std::function<void()> closed = {});
    uint16_t signalChannel(uint32_t channel);
    void service();
    bool needsService() const;
    std::string report() const;
  private:
    struct Reply { unsigned vp, sint; std::vector<uint8_t> payload; };
    std::deque<Reply> replies;
    std::span<uint8_t> ram;
    uint64_t base;
    unsigned cpus, targetVp = 0, targetSint = 2;
    ArmGpadl gpadls;
    struct Channel {
        Offer offer;
        PacketHandler handler;
        std::function<void()> closed;
        bool offered = false, work = false, notification = false;
        uint32_t ringGpadl = 0, vp = 0;
        std::unique_ptr<ArmVmbusRing> rx, tx;
        std::vector<uint8_t> pending;
    };
    std::map<uint32_t, Channel> channels;
    Event event;
    uint64_t channelOffers = 0, opens = 0, closes = 0, packetsIn = 0, packetsOut = 0, faults = 0;
    bool connected = false;
    uint32_t version = 0;
    uint64_t contacts = 0, offers = 0, unloads = 0, delivered = 0, rejected = 0;
    Send send;
    std::vector<uint8_t> offerMessage(const Offer &offer) const;
    void response(uint32_t kind, uint32_t channel, uint32_t id, uint32_t status);
    void close(Channel &channel);
    uint16_t channelMessage(uint32_t kind, std::span<const uint8_t> input);
    bool validPage(uint64_t gpa) const;
    uint16_t reject(uint16_t status);
};
}
