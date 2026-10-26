// VMBus is Hyper-V's device discovery and shared-memory channel bus. The guest
// first sends INITIATE_CONTACT through SynIC, negotiates a protocol version and
// response target, then requests channel offers. Each offer identifies a real
// host backend using interface/instance globally unique identifiers (GUIDs), a
// channel ID and an event connection. No default devices are invented here.
//
// To open a channel the guest describes shared RAM with a Guest Physical Address
// Descriptor List (GPADL), possibly split over control messages, then supplies the
// handle and page offset separating upstream/downstream rings. The host consumes
// guest packets and publishes backend replies through those rings. SynIC carries
// control replies and channel event bits; ArmGpadl validates RAM/ownership and
// ArmVmbusRing handles wrap, flow control and notification decisions. Close drops
// host ring state; GPADL teardown releases a mapping; unload closes all channels.
//
// IMPLEMENTATION NOTE: Calls/callbacks require all virtual processors stopped.
// Host configuration is frozen after contact; at most eight real channels/VPs and
// sixteen queued control replies are supported. Each channel has one retained
// backend response; bounded service passes retry full rings and failed event
// delivery without repeating a completed handler call. Bad ring state closes the
// affected channel. Metadata reports never expose packet content. Only virtual
// trust level zero, protocol 5.0..5.3/6.0, dedicated events and basic named pipes
// are supported; monitor pages are validated but no optional features negotiated.
//
// COMPATIBILITY: The ordinary connection uses fixed SINT2 even if the contact's
// target-info bytes are zero; bootstrap connection 4 enables explicit target
// fields. Full 240-byte Windows channel envelopes may contain dirty padding;
// decode only declared GPADL data and fixed message sizes. Copper OpenChannel2
// extra fields are ignored when their optional features were not negotiated.
// Client GUID presence is validated but this object still represents one client.
// These choices retain the observed Windows protocol behavior described in
// docs/hyperv-arm64.md; they do not implement all messages/features of a host bus.
// Primary wire reference: Microsoft's OpenVMM VMBus protocol definitions, pinned
// to revision 76420ea20a67ef65c80a62f1fc93de3b8f1eabaa, not a separate full TLFS bus spec.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L60
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L280
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L683
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L725

#include "arm_vmbus.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>
namespace pc {
namespace {
// VMBus control message header: type u32 and reserved-zero u32, included in
// every size/offset below. Message types are independent of the outer SynIC type.
// Microsoft OpenVMM MessageType and message structures, pinned revision.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L60
enum MessageType : uint32_t {
    OfferChannel = 1,
    RequestOffers = 3,
    AllOffersDelivered = 4,
    OpenChannel = 5,
    OpenResult = 6,
    CloseChannel = 7,
    GpadlHeader = 8,
    GpadlBody = 9,
    GpadlCreated = 10,
    GpadlTeardown = 11,
    GpadlTorndown = 12,
    InitiateContact = 14,
    VersionResponse = 15,
    Unload = 16,
    UnloadResponse = 17,
};
struct CommonField {
    enum : size_t { Type = 0, Reserved = 4, ChannelId = 8, RequestId = 12, Status = 16 };
};
constexpr size_t ControlHeaderBytes = 8, ChannelIdMessageBytes = 12, ChannelResponseBytes = 20,
                 MaximumControlMessageBytes = 240;
// Versions encode major in [31:16], minor in [15:0]. Support 5.0..5.3 and 6.0
// only, not every historical protocol or optional feature associated with them.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L320
constexpr uint32_t Version5_0 = 0x50000, Version5_3 = 0x50003, Version6_0 = 0x60000;
// INITIATE_CONTACT includes desired version/VP, target info and two monitor GPAs.
// Connection 1 ignores target info (fixed VTL0/SINT2); bootstrap connection 4
// enables target info. CLIENT_ID bit 3 adds a 16-byte GUID. Monitor pages remain unused.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L280
struct ContactField {
    enum : size_t {
        Version = 8,
        TargetVp = 12,
        TargetSint = 16,
        TargetVtl = 17,
        ReservedTarget = 18,
        Features = 20,
        MonitorPage1 = 24,
        MonitorPage2 = 32
    };
};
constexpr size_t ContactBytes = 40, ContactWithClientIdBytes = 56, VersionResponseBytes = 16,
                 VersionResponseWithFeaturesBytes = 20;
constexpr uint32_t ClientIdFeature = 8, MessageConnectionId = 1;
// TLFS allocates sixteen synthetic interrupt sources per VP.
constexpr unsigned SyntheticInterruptCount = 16;
// VERSION_RESPONSE returns supported u8, connection state u8 (zero success),
// padding u16, message connection u32, and for supported Copper (6.0) features u32.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L347
struct VersionResponseField {
    enum : size_t { Supported = 8, ConnectionId = 12 };
};
// OFFER_CHANNEL GUIDs/flags/user data, channel ID, dedicated-event bit and connection ID.
// Other fields stay zero: no monitored signaling, extra MMIO or subchannels.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L494
struct OfferField {
    enum : size_t {
        InterfaceId = 8,
        InstanceId = 24,
        Flags = 56,
        UserData = 60,
        ChannelId = 184,
        Dedicated = 190,
        ConnectionId = 192
    };
};
constexpr size_t OfferMessageBytes = 196;
constexpr uint16_t DedicatedEventPort = 1,
                   SupportedOfferFlags = 0x11; // Enumerate-device bit 0 + named-pipe bit 4.
// IMPLEMENTATION NOTE: Allocate explicit host connection IDs from 16; the outer
// Hyper-V connection ID field has 24 bits. Channel IDs must fit a SynIC event bitmap.
constexpr uint32_t FirstBackendConnectionId = 16, ConnectionIdMask = 0xffffff,
                   ChannelEventFlagCount = 2048;
// GPADL header: channel, handle, encoded range bytes u16, range count u16, ranges.
// Body: reserved/message number u32, handle u32, continuation PFNs. Both fragments
// contain complete eight-byte range/PFN units, bounded by the SynIC payload.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L683
struct GpadlField {
    enum : size_t { Handle = 12, RangeBytes = 16, RangeCount = 18, TornDownHandle = 8 };
};
constexpr size_t GpadlHeaderBytes = 20, GpadlBodyBytes = 16, GpadlTeardownBytes = 16,
                 GpadlTorndownBytes = 12, FragmentAlignmentMask = 7,
                 HeaderFragmentCapacity =
                     (MaximumControlMessageBytes - GpadlHeaderBytes) & ~FragmentAlignmentMask,
                 BodyFragmentCapacity = MaximumControlMessageBytes - GpadlBodyBytes;
// OPEN_CHANNEL carries channel/open IDs, GPADL, VP, downstream page offset,
// and 120 user bytes. OpenChannel2 adds connection/event/flags (8 bytes).
// VP index FFFFFFFFh requests no channel interrupts, rather than VP broadcast.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L715
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L725
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L749
struct OpenField {
    enum : size_t { Gpadl = 16, TargetVp = 20, DownstreamPageOffset = 24 };
};
constexpr size_t OpenChannelBytes = 148, OpenChannel2Bytes = 156;
constexpr uint32_t InterruptsDisabledVp = UINT32_MAX;
// The outer HV_MESSAGE type 1 identifies VMBus traffic; zero status means success.
// Hypercall statuses identify invalid input/state, resource pressure, VP or VTL denial.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_message_type
// https://github.com/torvalds/linux/blob/master/include/hyperv/hvgdk_mini.h
constexpr uint32_t VmbusSynicMessageType = 1;
constexpr uint16_t InvalidParameter = 5, InvalidVirtualProcessor = 14, AccessDenied = 6,
                   InsufficientBuffers = 0x13, InvalidVpState = 0x15;
// Channel reply NTSTATUS is distinct from a hypercall result: invalid data or
// exhausted host descriptor storage. MS-ERREF, NTSTATUS values.
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-49e60bebca55
constexpr uint32_t StatusInvalidParameter = 0xc000000d, StatusInsufficientResources = 0xc000009a;
// Packet descriptor length is u16 units of eight bytes; bound backend responses
// before retaining them to avoid unbounded host allocations downstream.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L357
constexpr size_t MaximumChannelPacketBytes = size_t(UINT16_MAX) * 8;
// Decode a little-endian control field after its enclosing message bounds were checked.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L280
uint64_t read_little_endian(std::span<const uint8_t> bytes, size_t at, unsigned size) {
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= uint64_t(bytes[at + i]) << (8 * i);
    return value;
}
// Encode one little-endian field into pre-sized zero-initialized control reply storage.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L347
void write_little_endian(std::span<uint8_t> bytes, size_t at, uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
        bytes[at + i] = uint8_t(value >> (8 * i));
}
// Accept the explicitly implemented protocol range 5.0..5.3 or Copper 6.0 only.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L320
bool supported_version(uint32_t version) {
    return (version >= Version5_0 && version <= Version5_3) || version == Version6_0;
}
} // namespace
// Bind RAM/mapping storage and reject zero or unsupported host VP counts.
// IMPLEMENTATION NOTE: The host inventory is capped independently of protocol IDs.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L280
ArmVmbus::ArmVmbus(std::span<uint8_t> memory, uint64_t start, unsigned count)
    : ram(memory), base(start), cpus(count), gpadls(memory, start) {
    if (!count || count > MaximumVirtualProcessors)
        throw std::runtime_error("Invalid VMBus VP count");
}
// Accept an unused zero monitor GPA or one complete aligned page in guest RAM.
// IMPLEMENTATION NOTE: Pages are validated but monitor signaling is not negotiated.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L280
bool ArmVmbus::valid_monitor_page(uint64_t gpa) const {
    return !gpa ||
           (!(gpa & ArmGpadl::GuestPageOffsetMask) && gpa >= base && gpa - base <= ram.size() &&
            ArmGpadl::GuestPageBytes <= ram.size() - (gpa - base));
}
// Record one rejected control request and return its hypercall status unchanged.
// IMPLEMENTATION NOTE: Host diagnostics, separate from NTSTATUS values in control replies.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L60
uint16_t ArmVmbus::reject_message(uint16_t status) {
    ++rejected;
    return status;
}
// Validate a SynIC control envelope, negotiate contact, enumerate offers or unload the bus.
// Channel requests are dispatched only after contact; replies retain target VP/SINT routing.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L60
uint16_t ArmVmbus::receive_control_message(uint32_t messageType, std::span<const uint8_t> input,
                                           bool multiclient) {
    if (messageType != VmbusSynicMessageType || input.size() < ControlHeaderBytes ||
        input.size() > MaximumControlMessageBytes ||
        read_little_endian(input, CommonField::Reserved, 4))
        return reject_message(InvalidParameter);
    auto kind = read_little_endian(input, CommonField::Type, 4);
    if (kind != MessageType::InitiateContact && kind != MessageType::RequestOffers &&
        kind != MessageType::Unload) {
        if (!connected)
            return reject_message(InvalidVpState);
        if (replies.size() >= MaximumQueuedReplies)
            return reject_message(InsufficientBuffers);
        return handle_channel_message(uint32_t(kind), input);
    }
    if (replies.size() >= MaximumQueuedReplies)
        return reject_message(InsufficientBuffers);
    if (kind == MessageType::InitiateContact) {
        if (connected)
            return reject_message(InvalidVpState);
        if (input.size() < ContactBytes)
            return reject_message(InvalidParameter);
        auto requested = uint32_t(read_little_endian(input, ContactField::Version, 4));
        auto vp = read_little_endian(input, ContactField::TargetVp, 4);
        // The ordinary connection always uses VTL0/SINT2. Only connection 4
        // makes the lower target-info fields meaningful; Windows sends zero
        // there on connection 1 even though its active message source is SINT2.
        auto sint = multiclient ? read_little_endian(input, ContactField::TargetSint, 1)
                                : DefaultMessageSint;
        auto vtl = multiclient ? read_little_endian(input, ContactField::TargetVtl, 1) : 0;
        auto flags =
            requested >= Version6_0 ? read_little_endian(input, ContactField::Features, 4) : 0;
        if (vp >= cpus)
            return reject_message(InvalidVirtualProcessor);
        if (sint >= SyntheticInterruptCount ||
            (multiclient && read_little_endian(input, ContactField::ReservedTarget, 2)))
            return reject_message(InvalidParameter);
        if (vtl)
            return reject_message(AccessDenied);
        if (requested < Version5_0)
            return reject_message(
                InvalidParameter); // Legacy interrupt-page ABI is not implemented.
        if ((flags & ClientIdFeature) && input.size() < ContactWithClientIdBytes)
            return reject_message(InvalidParameter); // Optional client GUID.
        auto monitor1 = read_little_endian(input, ContactField::MonitorPage1, 8),
             monitor2 = read_little_endian(input, ContactField::MonitorPage2, 8);
        if (!valid_monitor_page(monitor1) || !valid_monitor_page(monitor2) ||
            (monitor1 && monitor1 == monitor2))
            return reject_message(InvalidParameter);
        bool accept = supported_version(requested);
        // Copper adds SupportedFeatures. Optional channel extensions and
        // monitor-page signaling are not negotiated by this implementation.
        std::vector<uint8_t> response(accept && requested >= Version6_0
                                          ? VersionResponseWithFeaturesBytes
                                          : VersionResponseBytes);
        write_little_endian(response, CommonField::Type, MessageType::VersionResponse, 4);
        response[VersionResponseField::Supported] = accept;
        if (accept)
            write_little_endian(response, VersionResponseField::ConnectionId, MessageConnectionId,
                                4); // Plain message connection ID 1.
        replies.push_back({unsigned(vp), unsigned(sint), std::move(response)});
        targetVp = unsigned(vp);
        targetSint = unsigned(sint);
        version = accept ? requested : 0;
        connected = accept;
        ++contacts;
    } else {
        if (!connected)
            return reject_message(InvalidVpState);
        if (input.size() != ControlHeaderBytes)
            return reject_message(InvalidParameter);
        if (kind == MessageType::RequestOffers) {
            if (channels.size() + 1 > MaximumQueuedReplies - replies.size())
                return reject_message(InsufficientBuffers);
            for (auto &[id, channel] : channels) {
                (void)id;
                replies.push_back({targetVp, targetSint, offer_message(channel.offer)});
                channel.offered = true;
                ++channelOffers;
            }
            ++offers;
        } else {
            for (auto &[id, channel] : channels) {
                close_channel(channel);
                gpadls.clear_channel(id);
                channel.offered = false;
            }
            connected = false;
            version = 0;
            ++unloads;
        }
        std::vector<uint8_t> response(ControlHeaderBytes);
        write_little_endian(response, CommonField::Type,
                            kind == MessageType::RequestOffers ? MessageType::AllOffersDelivered
                                                               : MessageType::UnloadResponse,
                            4);
        replies.push_back({targetVp, targetSint, std::move(response)});
    }
    return 0;
}
// Register a unique real host backend before contact, with bounded IDs and supported offer flags.
// IMPLEMENTATION NOTE: Require packet/event callbacks, nonzero GUIDs and no duplicate
// connection or GUID pair; callbacks must not reenter or mutate channel inventory.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L494
bool ArmVmbus::add_channel(const Offer &offer, PacketHandler handler, std::function<void()> closed,
                           std::function<void()> opened) {
    if (connected || !event || !handler || channels.size() >= MaximumChannels || !offer.channel ||
        offer.channel >= ChannelEventFlagCount || offer.connection < FirstBackendConnectionId ||
        offer.connection > ConnectionIdMask || (offer.flags & ~SupportedOfferFlags) ||
        channels.contains(offer.channel) ||
        std::all_of(offer.interfaceId.begin(), offer.interfaceId.end(),
                    [](auto b) { return !b; }) ||
        std::all_of(offer.instanceId.begin(), offer.instanceId.end(), [](auto b) { return !b; }))
        return false;
    for (const auto &[id, channel] : channels) {
        (void)id;
        if (channel.offer.connection == offer.connection ||
            (channel.offer.interfaceId == offer.interfaceId &&
             channel.offer.instanceId == offer.instanceId))
            return false;
    }
    Channel channel;
    channel.offer = offer;
    channel.handler = std::move(handler);
    channel.closed = std::move(closed);
    channel.opened = std::move(opened);
    channels.emplace(offer.channel, std::move(channel));
    return true;
}
// Encode an OFFER_CHANNEL with interface/instance GUIDs, user data and a dedicated event port.
// Zero-initialized reserved/MMIO/monitor/subchannel fields advertise no such resources.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L494
std::vector<uint8_t> ArmVmbus::offer_message(const Offer &offer) const {
    std::vector<uint8_t> message(OfferMessageBytes);
    write_little_endian(message, CommonField::Type, MessageType::OfferChannel, 4);
    std::copy(offer.interfaceId.begin(), offer.interfaceId.end(),
              message.begin() + OfferField::InterfaceId);
    std::copy(offer.instanceId.begin(), offer.instanceId.end(),
              message.begin() + OfferField::InstanceId);
    write_little_endian(message, OfferField::Flags, offer.flags, 2);
    std::copy(offer.userData.begin(), offer.userData.end(), message.begin() + OfferField::UserData);
    write_little_endian(message, OfferField::ChannelId, offer.channel, 4);
    write_little_endian(message, OfferField::Dedicated, DedicatedEventPort,
                        2); // Dedicated event port.
    write_little_endian(message, OfferField::ConnectionId, offer.connection, 4);
    return message;
}
// Queue an OPEN_RESULT or GPADL_CREATED with channel, request/handle ID and NTSTATUS.
// Caller has reserved bounded reply capacity before handling the request.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L709
void ArmVmbus::queue_channel_response(uint32_t kind, uint32_t channel, uint32_t id,
                                      uint32_t status) {
    std::vector<uint8_t> message(ChannelResponseBytes);
    write_little_endian(message, CommonField::Type, kind, 4);
    write_little_endian(message, CommonField::ChannelId, channel, 4);
    write_little_endian(message, CommonField::RequestId, id, 4);
    write_little_endian(message, CommonField::Status, status, 4);
    replies.push_back({targetVp, targetSint, std::move(message)});
}
// Release host ring views/pending work and invoke the close callback once for an open channel.
// GPADL descriptors remain until explicit teardown or bus unload.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L779
void ArmVmbus::close_channel(Channel &channel) {
    const bool wasOpen = bool(channel.rx);
    channel.rx.reset();
    channel.tx.reset();
    channel.pending.clear();
    channel.notification = channel.work = false;
    channel.ringGpadl = 0;
    if (wasOpen) {
        ++closes;
        if (channel.closed)
            channel.closed();
    }
}
// Mark an open channel for host ring service after a guest event; reject disconnected/closed IDs.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L725
uint16_t ArmVmbus::signal_channel(uint32_t id) {
    auto it = channels.find(id);
    if (!connected || it == channels.end() || !it->second.rx)
        return InvalidVpState;
    it->second.work = true;
    return 0;
}
// Publish host-initiated traffic to an open downstream ring and retain notification state.
// Full/pending responses leave caller ownership; malformed ring state closes the channel.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L725
uint16_t ArmVmbus::send_channel_packet(uint32_t id, std::span<const uint8_t> packet) {
    auto it = channels.find(id);
    if (!connected || it == channels.end() || !it->second.tx)
        return InvalidVpState;
    auto &channel = it->second;
    if (!channel.pending.empty())
        return InsufficientBuffers;
    bool notify = false;
    auto result = channel.tx->write_packet(packet, notify);
    if (result == ArmVmbusRing::Result::Full)
        return InsufficientBuffers;
    if (result != ArmVmbusRing::Result::Ok) {
        ++faults;
        close_channel(channel);
        return InvalidParameter;
    }
    channel.notification |= notify;
    ++packetsOut;
    return 0;
}
// Report queued replies, ring work, retained responses or undelivered notifications.
// IMPLEMENTATION NOTE: Host scheduling predicate; it performs no guest memory access.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L725
bool ArmVmbus::needs_service() const {
    if (!replies.empty())
        return true;
    for (const auto &[id, channel] : channels) {
        (void)id;
        if (channel.work || channel.notification || !channel.pending.empty())
            return true;
    }
    return false;
}
// Validate/handle channel open/close and GPADL fragments/teardown, queuing protocol replies.
// Compact messages are strict; retained Windows padded/OpenChannel2 compatibility is explained
// below. Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L683
uint16_t ArmVmbus::handle_channel_message(uint32_t kind, std::span<const uint8_t> input) {
    if (kind != MessageType::OpenChannel && kind != MessageType::CloseChannel &&
        kind != MessageType::GpadlHeader && kind != MessageType::GpadlBody &&
        kind != MessageType::GpadlTeardown)
        return reject_message(InvalidParameter);
    if (input.size() < ChannelIdMessageBytes)
        return reject_message(InvalidParameter);
    // Windows posts full 240-byte channel envelopes, including dirty unused
    // bytes. Decode only declared data; keep compact-message validation strict.
    if (input.size() == MaximumControlMessageBytes) {
        if (kind == MessageType::OpenChannel)
            input = input.first(OpenChannelBytes);
        else if (kind == MessageType::CloseChannel)
            input = input.first(ChannelIdMessageBytes);
        else if (kind == MessageType::GpadlTeardown)
            input = input.first(GpadlTeardownBytes);
        else if (kind == MessageType::GpadlHeader) {
            size_t bytes = size_t(read_little_endian(input, GpadlField::RangeBytes, 2));
            input = input.first(GpadlHeaderBytes + std::min(bytes, HeaderFragmentCapacity));
        } else if (kind == MessageType::GpadlBody) {
            auto remaining = gpadls.remaining_descriptor_bytes(
                uint32_t(read_little_endian(input, CommonField::RequestId, 4)));
            if (remaining && *remaining)
                input = input.first(GpadlBodyBytes + std::min(*remaining, BodyFragmentCapacity));
        }
    }
    uint32_t id = uint32_t(read_little_endian(input, CommonField::ChannelId, 4));
    if (kind == MessageType::GpadlBody) {
        if (input.size() <= GpadlBodyBytes ||
            ((input.size() - GpadlBodyBytes) & FragmentAlignmentMask))
            return reject_message(InvalidParameter);
        const uint32_t gpadl = uint32_t(read_little_endian(input, CommonField::RequestId, 4));
        const auto owner = gpadls.descriptor_owner(gpadl);
        if (!owner)
            return reject_message(InvalidParameter);
        id = *owner;
        auto result = gpadls.append_descriptor(gpadl, input.subspan(GpadlBodyBytes));
        if (result == ArmGpadl::Result::InUse)
            return reject_message(InvalidVpState);
        if (result != ArmGpadl::Result::Ok) {
            gpadls.teardown_descriptor(id, gpadl);
            queue_channel_response(MessageType::GpadlCreated, id, gpadl, StatusInvalidParameter);
        } else if (gpadls.mapped_byte_count(id, gpadl))
            queue_channel_response(MessageType::GpadlCreated, id, gpadl, 0);
        return 0;
    }
    auto it = channels.find(id);
    if (it == channels.end() || !it->second.offered)
        return reject_message(InvalidParameter);
    auto &channel = it->second;
    if (kind == MessageType::CloseChannel) {
        if (input.size() != ChannelIdMessageBytes)
            return reject_message(InvalidParameter);
        close_channel(channel);
        return 0;
    }
    if (kind == MessageType::GpadlHeader) {
        if (input.size() < GpadlHeaderBytes ||
            ((input.size() - GpadlHeaderBytes) & FragmentAlignmentMask))
            return reject_message(InvalidParameter);
        const uint32_t gpadl = uint32_t(read_little_endian(input, CommonField::RequestId, 4));
        auto result = gpadls.begin_descriptor(
            id, gpadl, uint16_t(read_little_endian(input, GpadlField::RangeBytes, 2)),
            uint16_t(read_little_endian(input, GpadlField::RangeCount, 2)),
            input.subspan(GpadlHeaderBytes));
        if (result != ArmGpadl::Result::Ok)
            queue_channel_response(MessageType::GpadlCreated, id, gpadl,
                                   result == ArmGpadl::Result::Limit ? StatusInsufficientResources
                                                                     : StatusInvalidParameter);
        else if (gpadls.mapped_byte_count(id, gpadl))
            queue_channel_response(MessageType::GpadlCreated, id, gpadl, 0);
        return 0;
    }
    if (kind == MessageType::GpadlTeardown) {
        if (input.size() != GpadlTeardownBytes)
            return reject_message(InvalidParameter);
        const uint32_t gpadl = uint32_t(read_little_endian(input, CommonField::RequestId, 4));
        if (!gpadl)
            return reject_message(InvalidParameter);
        if (channel.rx && channel.ringGpadl == gpadl)
            return reject_message(InvalidVpState);
        auto result = gpadls.teardown_descriptor(id, gpadl);
        if (result == ArmGpadl::Result::WrongChannel)
            return reject_message(InvalidParameter);
        std::vector<uint8_t> message(GpadlTorndownBytes);
        write_little_endian(message, CommonField::Type, MessageType::GpadlTorndown, 4);
        write_little_endian(message, GpadlField::TornDownHandle, gpadl, 4);
        replies.push_back({targetVp, targetSint, std::move(message)});
        return 0;
    }
    // Copper Windows may send the OpenChannel2-sized structure even when no
    // optional signal/redirection features were negotiated. Its eight extra
    // bytes have no meaning in that case; keep the legacy routing policy.
    if (version >= Version6_0 && input.size() == OpenChannel2Bytes)
        input = input.first(OpenChannelBytes);
    if (input.size() != OpenChannelBytes)
        return reject_message(InvalidParameter);
    const uint32_t openId = uint32_t(read_little_endian(input, CommonField::RequestId, 4)),
                   gpadl = uint32_t(read_little_endian(input, OpenField::Gpadl, 4));
    const uint32_t vp = uint32_t(read_little_endian(input, OpenField::TargetVp, 4));
    const uint64_t split =
        read_little_endian(input, OpenField::DownstreamPageOffset, 4) * ArmGpadl::GuestPageBytes;
    const auto size = gpadls.mapped_byte_count(id, gpadl);
    if (channel.rx || !size || *size > UINT32_MAX || split > *size ||
        (vp != InterruptsDisabledVp && vp >= cpus)) {
        queue_channel_response(MessageType::OpenResult, id, openId, StatusInvalidParameter);
        return 0;
    }
    try {
        auto rx = std::make_unique<ArmVmbusRing>(gpadls, id, gpadl, 0, uint32_t(split));
        auto tx = std::make_unique<ArmVmbusRing>(gpadls, id, gpadl, split, uint32_t(*size - split));
        channel.rx = std::move(rx);
        channel.tx = std::move(tx);
    } catch (const std::runtime_error &) {
        queue_channel_response(MessageType::OpenResult, id, openId, StatusInvalidParameter);
        return 0;
    }
    channel.vp = vp;
    channel.ringGpadl = gpadl;
    channel.work = true;
    ++opens;
    queue_channel_response(MessageType::OpenResult, id, openId, 0);
    if (channel.opened)
        channel.opened();
    return 0;
}
// Deliver queued control replies, service bounded ring transactions and retry notifications.
// IMPLEMENTATION NOTE: Retain one backend response per channel under downstream backpressure;
// malformed rings/oversized responses close only that channel. All guest VPs must be stopped.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L725
void ArmVmbus::service_channels() {
    while (send && !replies.empty()) {
        auto &reply = replies.front();
        if (send(reply.vp, reply.sint, reply.payload))
            break;
        auto kind = read_little_endian(reply.payload, CommonField::Type, 4);
        ++deliveredTypes[kind == MessageType::VersionResponse      ? 0
                         : kind == MessageType::OfferChannel       ? 1
                         : kind == MessageType::AllOffersDelivered ? 2
                         : kind == MessageType::UnloadResponse     ? 3
                                                                   : 4];
        replies.pop_front();
        ++delivered;
    }
    for (auto &[id, channel] : channels) {
        if (!channel.rx)
            continue;
        channel.work = false;
        for (unsigned budget = 0; budget < TransactionsPerService; ++budget) {
            if (!channel.pending.empty()) {
                bool notify = false;
                auto result = channel.tx->write_packet(channel.pending, notify);
                if (result == ArmVmbusRing::Result::Full)
                    break;
                if (result != ArmVmbusRing::Result::Ok) {
                    ++faults;
                    close_channel(channel);
                    break;
                }
                channel.notification |= notify;
                channel.pending.clear();
                ++packetsOut;
            }
            std::vector<uint8_t> packet;
            bool notify = false;
            auto result = channel.rx->read_packet(packet, notify);
            if (result == ArmVmbusRing::Result::Empty) {
                channel.work = false;
                break;
            }
            if (result != ArmVmbusRing::Result::Ok) {
                ++faults;
                close_channel(channel);
                break;
            }
            channel.notification |= notify;
            ++packetsIn;
            channel.pending = channel.handler(packet);
            if (channel.pending.size() > MaximumChannelPacketBytes) {
                ++faults;
                close_channel(channel);
                break;
            }
            channel.work = true; // A bounded follow-up also drains no-response streams.
        }
        if (channel.notification) {
            if (channel.vp == InterruptsDisabledVp || (event && !event(channel.vp, targetSint, id)))
                channel.notification = false;
        }
    }
}
// Serialize bounded bus/channel activity and lifecycle metadata without guest packet contents.
// IMPLEMENTATION NOTE: Host observability for contact/offers/GPADL/ring transitions.
// Microsoft OpenVMM VMBus protocol wire definitions (pinned revision).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L60
std::string ArmVmbus::report() const {
    std::ostringstream out;
    size_t offered = 0, open = 0;
    for (const auto &[id, channel] : channels) {
        (void)id;
        offered += channel.offered;
        open += bool(channel.rx);
    }
    out << "{\"connected\":" << (connected ? "true" : "false") << ",\"version\":" << version
        << ",\"targetVp\":" << targetVp << ",\"targetSint\":" << targetSint
        << ",\"contacts\":" << contacts << ",\"offerRequests\":" << offers
        << ",\"unloads\":" << unloads << ",\"deliveredReplies\":" << delivered
        << ",\"pendingReplies\":" << replies.size()
        << ",\"deliveredContactReplies\":" << deliveredTypes[0]
        << ",\"deliveredOfferReplies\":" << deliveredTypes[1]
        << ",\"deliveredOffersDoneReplies\":" << deliveredTypes[2]
        << ",\"deliveredUnloadReplies\":" << deliveredTypes[3]
        << ",\"deliveredChannelReplies\":" << deliveredTypes[4]
        << ",\"rejectedMessages\":" << rejected << ",\"offeredChannels\":" << offered
        << ",\"openChannels\":" << open << ",\"channelOffers\":" << channelOffers
        << ",\"channelOpens\":" << opens << ",\"channelCloses\":" << closes
        << ",\"gpadls\":" << gpadls.descriptor_count() << ",\"packetsReceived\":" << packetsIn
        << ",\"packetsSent\":" << packetsOut << ",\"channelFaults\":" << faults << '}';
    return out.str();
}
} // namespace pc
