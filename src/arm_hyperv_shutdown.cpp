// The shutdown Integration Component (IC) carries an explicit host request for
// guest power-off, restart or hibernation. Opening its VMBus message-pipe channel
// only negotiates framework 1.0/3.0 and shutdown service 3.2; it never requests a
// power operation by itself. After negotiation, request_shutdown stores one
// action/force/guest-timeout tuple. The packet includes a planned reason code,
// guest timeout in seconds, action flags and an empty friendly-message buffer.
//
// ArmVmbus owns transport and notifications; packet_published starts the host's
// response deadline only after a packet reaches the ring. A matching successful
// response marks the request Accepted, not completed guest shutdown. The board
// observes actual guest power transitions separately. A guest denial permits a
// new explicit request; malformed/stale replies do not complete the pending one.
//
// IMPLEMENTATION NOTE: All guest virtual processors must be stopped during calls.
// One operation at a time, no automatic retries. Thirty host seconds without a
// response fail the session until reopen; this differs from the independent
// guest timeout value forwarded in seconds. Transaction IDs increment on replies.
// Closing drops pending state; host diagnostic counters survive. The wire supports
// a friendly message, but this interface supplies zeros, and offers only v3.2.
// Microsoft OpenVMM primary protocol (pinned revision), ShutdownMessage,
// ShutdownFlags and SHTDN_REASON_FLAG_PLANNED; framing in arm_hyperv_ic_protocol.hpp.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L34
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L56

#include "arm_hyperv_shutdown.hpp"
#include <sstream>
namespace pc {
namespace {
using namespace hyperv_ic;
// ShutdownMessage: planned reason, guest timeout in seconds, flags, then 2048
// zeroed friendly-message bytes. No text is supplied by this host request path.
// Flags: bit 0 force, bit 1 guest restart, bit 2 guest hibernate; zero means power off.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L34
struct ShutdownField {
    enum : size_t { ReasonCode = 0, TimeoutSeconds = 4, Flags = 8, Message = 12 };
};
constexpr uint32_t PlannedShutdownReason = 0x80000000, ForceShutdown = 1, RestartGuest = 2,
                   HibernateGuest = 4;
constexpr uint16_t ShutdownVersionMajor = 3, ShutdownVersionMinor = 2;
constexpr unsigned ShutdownBodyBytes = ShutdownField::Message + 2048,
                   NegotiateBodyBytes = NegotiateHeaderBytes + 3 * VersionBytes;
// Distinct GUID identities, encoded with little-endian first three components.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L15
constexpr std::array<uint8_t, 16> ShutdownInterfaceId{
    0x31, 0x60, 0x0b, 0x0e, 0x13, 0x52, 0x34, 0x49, 0x81, 0x8b, 0x38, 0xd9, 0x0c, 0xed, 0x39, 0xdb};
constexpr std::array<uint8_t, 16> ShutdownInstanceId{
    0xf7, 0x0f, 0x65, 0xb6, 0xbc, 0x33, 0x40, 0x48, 0x80, 0x48, 0xe0, 0x67, 0x67, 0x86, 0xf3, 0x93};
} // namespace
// Build the shutdown named-pipe offer using its distinct interface/instance GUIDs.
// Microsoft OpenVMM ShutdownMessage / ShutdownFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L15
ArmVmbus::Offer ArmHyperVShutdown::channel_offer() {
    ArmVmbus::Offer o{};
    o.channel = ChannelId;
    o.connection = ConnectionId;
    o.flags = IntegrationChannelFlags;
    o.userData[0] = MessagePipeType;
    o.interfaceId = ShutdownInterfaceId;
    o.instanceId = ShutdownInstanceId;
    return o;
}
// Begin negotiation and reset request flags, transaction and status; preserve lifetime counters.
// Microsoft OpenVMM ShutdownMessage / ShutdownFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L34
void ArmHyperVShutdown::channel_opened(uint64_t now) {
    state = State::SendVersion;
    framework = 0;
    transaction = 0;
    flags = guestTimeout = lastStatus = 0;
    since = now;
}
// Stop session activity and discard uncompleted request parameters; do not trigger a power
// operation. Microsoft OpenVMM ShutdownMessage / ShutdownFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L34
void ArmHyperVShutdown::channel_closed() {
    state = State::Closed;
    framework = 0;
    flags = guestTimeout = 0;
}
// Queue an explicit action only in Ready state; encode force/restart/hibernate flags and guest
// seconds. IMPLEMENTATION NOTE: Reject concurrent/unnegotiated actions; no automatic request
// follows open. Microsoft OpenVMM ShutdownMessage / ShutdownFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L56
bool ArmHyperVShutdown::request_shutdown(Action action, bool force, uint32_t seconds) {
    if (state != State::Ready)
        return false;
    uint32_t mode = 0;
    switch (action) {
    case Action::PowerOff:
        break;
    case Action::Reboot:
        mode = RestartGuest;
        break;
    case Action::Hibernate:
        mode = HibernateGuest;
        break;
    default:
        return false;
    }
    flags = mode | (force ? ForceShutdown : 0);
    guestTimeout = seconds;
    state = State::SendRequest;
    return true;
}
// Build the due version or explicit shutdown message; missing replies fail until channel reopen.
// Publication ownership is separate, so a full ring does not count or time an unsent operation.
// Microsoft OpenVMM ShutdownMessage / ShutdownFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L34
std::vector<uint8_t> ArmHyperVShutdown::pending_packet(uint64_t now) {
    if ((state == State::WaitVersion || state == State::WaitResponse) &&
        now - since >= ResponseTimeout100ns) {
        state = State::Failed;
        ++timeouts;
    }
    const bool negotiate = state == State::SendVersion;
    if (!negotiate && state != State::SendRequest)
        return {};
    const unsigned body = negotiate ? NegotiateBodyBytes : ShutdownBodyBytes;
    std::vector<uint8_t> p((BodyOffset + body + PacketAlignmentMask) & ~PacketAlignmentMask);
    write_little_endian(p, PacketField::Type, InBandPacketType, 2);
    write_little_endian(p, PacketField::DataOffset8, InBandDataOffset8, 2);
    write_little_endian(p, PacketField::Length8, p.size() / PacketUnitBytes, 2);
    write_little_endian(p, WireField::PipeType, PipeDataPacketType, 4);
    write_little_endian(p, WireField::PipeMessageBytes, HeaderBytes + body, 4);
    write_little_endian(p, WireField::FrameworkMajor, negotiate ? 0 : framework, 2);
    write_little_endian(p, WireField::MessageType,
                        negotiate ? MessageType::Negotiate : MessageType::Shutdown, 2);
    write_little_endian(p, WireField::MessageMajor, negotiate ? 0 : ShutdownVersionMajor, 2);
    write_little_endian(p, WireField::MessageMinor, negotiate ? 0 : ShutdownVersionMinor, 2);
    write_little_endian(p, WireField::BodyBytes, body, 2);
    p[WireField::TransactionId] = transaction;
    p[WireField::Flags] = TransactionRequest;
    if (negotiate) {
        write_little_endian(p, BodyOffset + NegotiateField::FrameworkCount, 2, 2);
        write_little_endian(p, BodyOffset + NegotiateField::ServiceCount, 1, 2);
        write_little_endian(p, BodyOffset + NegotiateField::OfferedFramework1Major,
                            FrameworkVersion1, 2);
        write_little_endian(p, BodyOffset + NegotiateField::OfferedFramework3Major,
                            FrameworkVersion3, 2);
        write_little_endian(p, BodyOffset + NegotiateField::OfferedService1Major,
                            ShutdownVersionMajor, 2);
        write_little_endian(p, BodyOffset + NegotiateField::OfferedService1Minor,
                            ShutdownVersionMinor, 2);
    } else {
        write_little_endian(p, BodyOffset + ShutdownField::ReasonCode, PlannedShutdownReason, 4);
        write_little_endian(p, BodyOffset + ShutdownField::TimeoutSeconds, guestTimeout, 4);
        write_little_endian(p, BodyOffset + ShutdownField::Flags, flags, 4);
    }
    return p;
}
// Start the negotiation/request response deadline only after successful ring publication.
// Microsoft OpenVMM ShutdownMessage / ShutdownFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L34
void ArmHyperVShutdown::packet_published(uint64_t now) {
    if (state == State::SendVersion) {
        state = State::WaitVersion;
        ++negotiations;
    } else if (state == State::SendRequest) {
        state = State::WaitResponse;
        ++requests;
    } else
        return;
    since = now;
}
// Validate framing, current transaction and selected versions, then record guest status.
// Zero status enters Accepted; denial returns Ready for a new explicit request. Neither
// acknowledgment proves completed shutdown nor causes host-side power/reset here.
// Microsoft OpenVMM ShutdownMessage / ShutdownFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L34
void ArmHyperVShutdown::receive_response(std::span<const uint8_t> p, uint64_t now) {
    auto fail = [&] { ++rejected; };
    if ((state == State::WaitVersion || state == State::WaitResponse) &&
        now - since >= ResponseTimeout100ns) {
        state = State::Failed;
        ++timeouts;
    }
    if (p.size() < PacketDescriptorBytes ||
        read_little_endian(p, PacketField::Type, 2) != InBandPacketType ||
        read_little_endian(p, PacketField::Length8, 2) * PacketUnitBytes != p.size())
        return fail();
    auto offset = read_little_endian(p, PacketField::DataOffset8, 2) * PacketUnitBytes;
    if (offset < PacketDescriptorBytes || offset > p.size() ||
        p.size() - offset < PipeHeaderBytes + HeaderBytes)
        return fail();
    auto pipe = p.subspan(size_t(offset));
    auto size = read_little_endian(pipe, PipeField::MessageBytes, 4);
    if (read_little_endian(pipe, PipeField::Type, 4) != PipeDataPacketType || size < HeaderBytes ||
        size > pipe.size() - PipeHeaderBytes)
        return fail();
    auto h = pipe.subspan(PipeHeaderBytes, size_t(size));
    auto bytes = read_little_endian(h, HeaderField::BodyBytes, 2);
    if (bytes > h.size() - HeaderBytes || h[HeaderField::TransactionId] != transaction ||
        h[HeaderField::Flags] != TransactionResponse)
        return fail();
    auto body = h.subspan(HeaderBytes, size_t(bytes));
    if (state == State::WaitVersion &&
        read_little_endian(h, HeaderField::MessageType, 2) == MessageType::Negotiate) {
        if (read_little_endian(h, HeaderField::Status, 4) || body.size() < SelectedVersionsBytes ||
            read_little_endian(body, NegotiateField::FrameworkCount, 2) != 1 ||
            read_little_endian(body, NegotiateField::ServiceCount, 2) != 1 ||
            read_little_endian(body, NegotiateField::FrameworkMinor, 2) ||
            read_little_endian(body, NegotiateField::ServiceMajor, 2) != ShutdownVersionMajor ||
            read_little_endian(body, NegotiateField::ServiceMinor, 2) != ShutdownVersionMinor)
            return fail();
        auto fw = read_little_endian(body, NegotiateField::FrameworkMajor, 2);
        if (fw != FrameworkVersion1 && fw != FrameworkVersion3)
            return fail();
        framework = uint16_t(fw);
        ++transaction;
        state = State::Ready;
        return;
    }
    if (state != State::WaitResponse ||
        read_little_endian(h, HeaderField::FrameworkMajor, 2) != framework ||
        read_little_endian(h, HeaderField::FrameworkMinor, 2) ||
        read_little_endian(h, HeaderField::MessageType, 2) != MessageType::Shutdown ||
        read_little_endian(h, HeaderField::MessageMajor, 2) != ShutdownVersionMajor ||
        read_little_endian(h, HeaderField::MessageMinor, 2) != ShutdownVersionMinor)
        return fail();
    lastStatus = uint32_t(read_little_endian(h, HeaderField::Status, 4));
    ++transaction;
    if (lastStatus) {
        ++denied;
        state = State::Ready;
    } else {
        ++accepted;
        state = State::Accepted;
    }
}
// Return ticks until pending work or a reply deadline; accepted/failed/closed sessions have no
// timer. IMPLEMENTATION NOTE: Deadline policy is thirty host seconds, independent of guestTimeout.
// Microsoft OpenVMM ShutdownMessage / ShutdownFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L34
std::optional<uint64_t> ArmHyperVShutdown::next_delay_100ns(uint64_t now) const {
    if (state == State::SendVersion || state == State::SendRequest)
        return 0;
    if (state == State::WaitVersion || state == State::WaitResponse)
        return now - since >= ResponseTimeout100ns ? 0 : ResponseTimeout100ns - (now - since);
    return {};
}
// Report bounded request/response counters, raw guest status and decoded request parameters.
// Microsoft OpenVMM ShutdownMessage / ShutdownFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/shutdown.rs#L56
std::string ArmHyperVShutdown::report() const {
    const char *name = state == State::Closed         ? "closed"
                       : state == State::SendVersion  ? "send-version"
                       : state == State::WaitVersion  ? "wait-version"
                       : state == State::Ready        ? "ready"
                       : state == State::SendRequest  ? "send-request"
                       : state == State::WaitResponse ? "wait-response"
                       : state == State::Accepted     ? "accepted"
                                                      : "failed";
    std::ostringstream s;
    s << "{\"state\":\"" << name << "\",\"frameworkMajor\":" << framework
      << ",\"shutdownMajor\":" << (framework ? ShutdownVersionMajor : 0)
      << ",\"shutdownMinor\":" << (framework ? ShutdownVersionMinor : 0)
      << ",\"requestFlags\":" << flags << ",\"guestTimeoutSeconds\":" << guestTimeout
      << ",\"negotiations\":" << negotiations << ",\"requests\":" << requests
      << ",\"accepted\":" << accepted << ",\"denied\":" << denied
      << ",\"lastStatus\":" << lastStatus << ",\"rejectedResponses\":" << rejected
      << ",\"timeouts\":" << timeouts << "}";
    return s.str();
}
} // namespace pc
