#pragma once
#include "arm_vmbus_ring.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
namespace pc {
// Virtual trust level zero (VTL0) VMBus control and registered-channel transport.
// Microsoft OpenVMM protocol definitions (pinned revision), primary wire reference:
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L60
// No default synthetic channel inventory; host backends explicitly register offers.
// Invoked only with guest VPs stopped, like ArmHyperV/SynIC.
class ArmVmbus {
  public:
    // IMPLEMENTATION NOTE: Host inventory/reply/service budgets, not protocol maxima.
    static constexpr unsigned MaximumVirtualProcessors = 8, MaximumChannels = 8,
                              MaximumQueuedReplies = 16, TransactionsPerService = 16;
    // SynIC's standard VMBus source is SINT2; GUIDs are 16 bytes and channel
    // offers reserve 120 user-defined bytes for the backend-specific wire protocol.
    // https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L298
    static constexpr unsigned DefaultMessageSint = 2, GuidBytes = 16, UserDataBytes = 120;
    using Send = std::function<uint16_t(unsigned, unsigned, std::span<const uint8_t>)>;
    // Bind RAM/GPADL ownership and a bounded nonempty VP inventory; throw on invalid count.
    ArmVmbus(std::span<uint8_t> ram, uint64_t base, unsigned cpus);
    // Bind host SynIC message posting; zero status means successful publication.
    // IMPLEMENTATION NOTE: Host transport wiring, with all guest VPs stopped.
    // https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L725
    void set_message_sender(Send sender) {
        send = std::move(sender);
    }
    // Connection 1 is the legacy single-client bus (fixed SINT2).
    // Bootstrap connection 4 opts into the target-info/multiclient ABI.
    // Validate a SynIC control envelope, negotiate contact, enumerate offers or unload the bus.
    uint16_t receive_control_message(uint32_t messageType, std::span<const uint8_t> payload,
                                     bool multiclient = false);
    struct Offer {
        uint32_t channel, connection;
        std::array<uint8_t, GuidBytes> interfaceId, instanceId;
        uint16_t flags = 0;
        std::array<uint8_t, UserDataBytes> userData{};
    };
    // Returns a complete wire packet, or an empty vector for no response.
    // Trusted backends must not reenter the bus or change its channel inventory.
    using PacketHandler = std::function<std::vector<uint8_t>(std::span<const uint8_t>)>;
    using Event = std::function<uint16_t(unsigned, unsigned, unsigned)>;
    // Bind host SynIC channel-event delivery; zero status means successful publication.
    // IMPLEMENTATION NOTE: Host transport wiring, with all guest VPs stopped.
    // https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L725
    void set_event_sender(Event sender) {
        event = std::move(sender);
    }
    // Trusted host configuration, before contact. Max eight real backends.
    // Register a unique real host backend before contact, with bounded IDs and supported offer
    // flags.
    bool add_channel(const Offer &offer, PacketHandler handler, std::function<void()> closed = {},
                     std::function<void()> opened = {});
    // Mark an open channel for host ring service after a guest event; reject disconnected/closed
    // IDs.
    uint16_t signal_channel(uint32_t channel);
    // Host initiated in-band traffic. Full leaves the packet with its caller.
    // Publish host-initiated traffic to an open downstream ring and retain notification state.
    uint16_t send_channel_packet(uint32_t channel, std::span<const uint8_t> packet);
    // Deliver queued control replies, service bounded ring transactions and retry notifications.
    void service_channels();
    // Report queued replies, ring work, retained responses or undelivered notifications.
    bool needs_service() const;
    // Serialize bounded bus/channel activity and lifecycle metadata without guest packet contents.
    std::string report() const;

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    struct Reply {
        unsigned vp, sint;
        std::vector<uint8_t> payload;
    };
    std::deque<Reply> replies;
    std::span<uint8_t> ram;
    uint64_t base;
    unsigned cpus, targetVp = 0, targetSint = DefaultMessageSint;
    ArmGpadl gpadls;
    struct Channel {
        Offer offer;
        PacketHandler handler;
        std::function<void()> closed, opened;
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
    std::array<uint64_t, 5> deliveredTypes{}; // contact, offer, offers-done, unload, channel
    Send send;
    // Encode an OFFER_CHANNEL with interface/instance GUIDs, user data and a dedicated event port.
    std::vector<uint8_t> offer_message(const Offer &offer) const;
    // Queue an OPEN_RESULT or GPADL_CREATED with channel, request/handle ID and NTSTATUS.
    void queue_channel_response(uint32_t kind, uint32_t channel, uint32_t id, uint32_t status);
    // Release host ring views/pending work and invoke the close callback once for an open channel.
    void close_channel(Channel &channel);
    // Validate/handle channel open/close and GPADL fragments/teardown, queuing protocol replies.
    uint16_t handle_channel_message(uint32_t kind, std::span<const uint8_t> input);
    // Accept an unused zero monitor GPA or one complete aligned page in guest RAM.
    bool valid_monitor_page(uint64_t gpa) const;
    // Record one rejected control request and return its hypercall status unchanged.
    uint16_t reject_message(uint16_t status);
};
} // namespace pc
