// Heartbeat checks whether a guest integration service can answer host messages.
// This integration component (IC) offers a VMBus message-pipe channel, negotiates framework and
// heartbeat service versions (1.0 or 3.0), then sends a sequence-number probe.
// A valid reply increments that sequence; v3 may also report application health.
// Negotiation or queued packets alone never prove that the guest is responsive.
//
// The full packet nests a VMBus descriptor, pipe DATA header, twenty-byte IC
// header and heartbeat body, padded to eight bytes. ArmVmbus owns rings and SynIC
// notifications. This component owns negotiation, probe correlation and deadlines.
// Call packet_published only after actual ring publication: retries retain the
// candidate without starting its response timer or counting a probe early.
//
// IMPLEMENTATION NOTE: All guest virtual processors are stopped during calls.
// Probe five seconds after a valid reply, expire after thirty seconds, retire the
// old sequence and retry without corrective guest power/reset actions. Negotiation
// timeout fails until channel reopen. Counters/sequence survive close; liveness
// does not. COMPATIBILITY: Keep the forty-byte Linux legacy heartbeat body and
// tolerate extra declared negotiation tail bytes while validating selected fields.
// A healthy transport does not imply a healthy application; report both separately.
// Primary wire references: Microsoft OpenVMM IC Header/NegotiateMessage and
// HeartbeatMessage, plus Linux heartbeat_msg_data for the legacy body.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L89
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/heartbeat.rs#L15
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L1595

#include "arm_hyperv_heartbeat.hpp"
#include <algorithm>
#include <sstream>
namespace pc {
namespace {
using namespace hyperv_ic;
// Heartbeat sequence occupies the first eight bytes. Version 3 returns an
// application state at +8 (unknown/healthy/critical/stopped = 0/1/2/3).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/heartbeat.rs#L15
struct HeartbeatField {
    enum : size_t { Sequence = 0, ApplicationState = 8 };
};
constexpr uint16_t HeartbeatVersion1 = 1, HeartbeatVersion3 = 3;
// Host requests use even sequences, guest replies increment by one. Advancing
// by two retires the old request/reply pair, including timed-out transactions.
constexpr uint64_t NextProbeSequenceStep = 2;
// COMPATIBILITY: Use Linux's forty-byte legacy body with a zeroed reserved tail;
// OpenVMM describes a shorter v3 body. Read only the returned declared bytes.
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L1595
constexpr unsigned LegacyHeartbeatBodyBytes = 40;
constexpr unsigned NegotiateBodyBytes = NegotiateHeaderBytes + 4 * VersionBytes;
// Interface GUID identifies the heartbeat service. Instance GUID is the Windows
// transport's distinct identity, observed from matching icsvc.dll/public PDB data.
// COMPATIBILITY: Keep this pair so the Windows client opens the offered channel.
// Repository evidence: docs/hyperv-arm64.md, Heartbeat instance GUID correction.
constexpr std::array<uint8_t, 16> HeartbeatInterfaceId{
    0x39, 0x4f, 0x16, 0x57, 0x15, 0x91, 0x78, 0x4e, 0xab, 0x55, 0x38, 0x2f, 0x3b, 0xd5, 0x42, 0x2d};
constexpr std::array<uint8_t, 16> HeartbeatInstanceId{
    0x91, 0x9e, 0x14, 0xfd, 0xe0, 0x82, 0x7d, 0x4a, 0xaf, 0xa6, 0x2a, 0x41, 0x66, 0xcb, 0xd7, 0xc0};
} // namespace
// Build the explicit heartbeat named-pipe channel offer; IDs are host allocation, GUIDs identify
// service.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L89
ArmVmbus::Offer ArmHyperVHeartbeat::channel_offer() {
    ArmVmbus::Offer o{};
    o.channel = ChannelId;
    o.connection = ConnectionId;
    o.flags = IntegrationChannelFlags;
    o.interfaceId = HeartbeatInterfaceId;
    // Windows ICTransportVMBusClient filters notifications by the heartbeat
    // instance GUID as well as the interface GUID (they are different).
    o.instanceId = HeartbeatInstanceId;
    o.userData[0] = MessagePipeType; // Message-mode named pipe.
    return o;
}
// Begin negotiation, clearing session liveness and versions; keep lifetime sequence/counters.
// IMPLEMENTATION NOTE: Retaining sequence prevents an old response satisfying a reopened session.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/heartbeat.rs
void ArmHyperVHeartbeat::channel_opened(uint64_t now) {
    state = State::SendVersion;
    responsive = expired = unresponsive = false;
    framework = version = 0;
    applicationState = 0;
    since = now;
}
// Stop probing and clear session liveness/version state without changing lifetime counters.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/heartbeat.rs
void ArmHyperVHeartbeat::channel_closed() {
    state = State::Closed;
    responsive = expired = unresponsive = false;
    framework = version = 0;
    applicationState = 0;
}
// Build a due negotiation/probe; expire unanswered transactions and retire timed-out probe
// sequences. IMPLEMENTATION NOTE: Publication is separate so a full ring cannot count an unsent
// probe.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L89
std::vector<uint8_t> ArmHyperVHeartbeat::pending_packet(uint64_t now) {
    if ((state == State::WaitVersion || state == State::WaitHeartbeat) && !expired &&
        now - since >= ResponseTimeout100ns) {
        ++timeouts;
        expired = true;
        responsive = false;
        unresponsive = true;
        if (state == State::WaitVersion)
            state = State::Failed;
        else
            sequence += NextProbeSequenceStep; // Retire the timed-out transaction before retrying.
    }
    const bool negotiate = state == State::SendVersion;
    if (!negotiate && !(state == State::Ready && now - since >= ProbeInterval100ns) &&
        !(state == State::WaitHeartbeat && expired))
        return {};
    const unsigned body =
        negotiate ? NegotiateBodyBytes
                  : LegacyHeartbeatBodyBytes; // Legacy reserved tail is zero; v3 state is at +8.
    std::vector<uint8_t> p((BodyOffset + body + PacketAlignmentMask) & ~PacketAlignmentMask);
    write_little_endian(p, PacketField::Type, InBandPacketType, 2);
    write_little_endian(p, PacketField::DataOffset8, InBandDataOffset8, 2);
    write_little_endian(p, PacketField::Length8, p.size() / PacketUnitBytes,
                        2); // In-band, no completion requested.
    write_little_endian(p, WireField::PipeType, PipeDataPacketType, 4);
    write_little_endian(p, WireField::PipeMessageBytes, HeaderBytes + body,
                        4); // Message pipe header.
    write_little_endian(p, WireField::FrameworkMajor, negotiate ? 0 : framework, 2);
    write_little_endian(p, WireField::MessageType,
                        negotiate ? MessageType::Negotiate : MessageType::Heartbeat, 2);
    write_little_endian(p, WireField::MessageMajor, negotiate ? 0 : version, 2);
    write_little_endian(p, WireField::BodyBytes, body, 2);
    p[WireField::Flags] = TransactionRequest; // IC transaction/request.
    if (negotiate) {
        write_little_endian(p, BodyOffset + NegotiateField::FrameworkCount, 2, 2);
        write_little_endian(p, BodyOffset + NegotiateField::ServiceCount, 2, 2);
        write_little_endian(p, BodyOffset + NegotiateField::OfferedFramework1Major,
                            FrameworkVersion1, 2);
        write_little_endian(p, BodyOffset + NegotiateField::OfferedFramework3Major,
                            FrameworkVersion3,
                            2); // Framework 1.0, 3.0.
        write_little_endian(p, BodyOffset + NegotiateField::OfferedService1Major, HeartbeatVersion1,
                            2);
        write_little_endian(p, BodyOffset + NegotiateField::OfferedService2Major, HeartbeatVersion3,
                            2); // Heartbeat 1.0, 3.0.
    } else
        write_little_endian(p, BodyOffset + HeartbeatField::Sequence, sequence, 8);
    return p;
}
// Start the response deadline/count only after successful ring publication; clear the expired
// latch.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L89
void ArmHyperVHeartbeat::packet_published(uint64_t now) {
    if (state == State::SendVersion) {
        state = State::WaitVersion;
        ++negotiations;
    } else if (state == State::Ready || (state == State::WaitHeartbeat && expired)) {
        state = State::WaitHeartbeat;
        ++probes;
    } else
        return;
    since = now;
    expired = false;
}
// Validate packet/pipe/IC lengths, flags, negotiated versions and the incremented current sequence.
// Negotiation selects one offered framework/service pair; it does not establish responsiveness.
// A successful heartbeat records transport liveness; v3 application state is independent.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/heartbeat.rs#L15
void ArmHyperVHeartbeat::receive_response(std::span<const uint8_t> p, uint64_t now) {
    auto fail = [&] { ++rejected; };
    if (p.size() < PacketDescriptorBytes ||
        read_little_endian(p, PacketField::Type, 2) != InBandPacketType ||
        read_little_endian(p, PacketField::Length8, 2) * PacketUnitBytes != p.size())
        return fail();
    const auto offset = read_little_endian(p, PacketField::DataOffset8, 2) * PacketUnitBytes;
    if (offset < PacketDescriptorBytes || offset > p.size() ||
        p.size() - offset < PipeHeaderBytes + HeaderBytes)
        return fail();
    auto pipe = p.subspan(size_t(offset));
    auto size = read_little_endian(pipe, PipeField::MessageBytes, 4);
    if (read_little_endian(pipe, PipeField::Type, 4) != PipeDataPacketType || size < HeaderBytes ||
        size > pipe.size() - PipeHeaderBytes)
        return fail();
    auto h = pipe.subspan(PipeHeaderBytes, size_t(size));
    auto bodySize = read_little_endian(h, HeaderField::BodyBytes, 2),
         type = read_little_endian(h, HeaderField::MessageType, 2);
    if (bodySize > h.size() - HeaderBytes || read_little_endian(h, HeaderField::Status, 4) ||
        h[HeaderField::TransactionId] || h[HeaderField::Flags] != TransactionResponse)
        return fail();
    auto body = h.subspan(HeaderBytes, size_t(bodySize));
    if (state == State::WaitVersion && type == MessageType::Negotiate) {
        if (body.size() < SelectedVersionsBytes ||
            read_little_endian(body, NegotiateField::FrameworkCount, 2) != 1 ||
            read_little_endian(body, NegotiateField::ServiceCount, 2) != 1 ||
            read_little_endian(body, NegotiateField::FrameworkMinor, 2) ||
            read_little_endian(body, NegotiateField::ServiceMinor, 2))
            return fail();
        auto fw = read_little_endian(body, NegotiateField::FrameworkMajor, 2),
             ver = read_little_endian(body, NegotiateField::ServiceMajor, 2);
        if ((fw != FrameworkVersion1 && fw != FrameworkVersion3) ||
            (ver != HeartbeatVersion1 && ver != HeartbeatVersion3))
            return fail();
        framework = uint16_t(fw);
        version = uint16_t(ver);
        state = State::Ready;
        // First probe is immediately due. Negotiation alone never proves liveness.
        since = now - ProbeInterval100ns;
        sequence += NextProbeSequenceStep;
        expired = false;
        return;
    }
    if (state != State::WaitHeartbeat || type != MessageType::Heartbeat ||
        body.size() < sizeof(uint64_t) ||
        read_little_endian(h, HeaderField::FrameworkMajor, 2) != framework ||
        read_little_endian(h, HeaderField::FrameworkMinor, 2) ||
        read_little_endian(h, HeaderField::MessageMajor, 2) != version ||
        read_little_endian(h, HeaderField::MessageMinor, 2) ||
        read_little_endian(body, HeartbeatField::Sequence, 8) != sequence + 1)
        return fail();
    applicationState = version == HeartbeatVersion3 &&
                               body.size() >= HeartbeatField::ApplicationState + sizeof(uint32_t)
                           ? uint32_t(read_little_endian(body, HeartbeatField::ApplicationState, 4))
                           : 0;
    ++responses;
    responsive = true;
    expired = unresponsive = false;
    since = now;
    sequence += NextProbeSequenceStep;
    state = State::Ready;
}
// Return 100 ns ticks until negotiation/probe/response-timeout work, or none when closed/failed.
// IMPLEMENTATION NOTE: Unsigned elapsed subtraction preserves deadlines across counter wrap.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L89
std::optional<uint64_t> ArmHyperVHeartbeat::next_delay_100ns(uint64_t now) const {
    if (state == State::SendVersion)
        return 0;
    if (state == State::Ready)
        return now - since >= ProbeInterval100ns ? 0 : ProbeInterval100ns - (now - since);
    if (state == State::WaitHeartbeat && expired)
        return 0;
    if ((state == State::WaitVersion || state == State::WaitHeartbeat) && !expired)
        return now - since >= ResponseTimeout100ns ? 0 : ResponseTimeout100ns - (now - since);
    return {};
}
// Report bounded protocol counters, transport responsiveness and raw v3 application state.
// IMPLEMENTATION NOTE: No automatic recovery/reset action follows a missed heartbeat.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/heartbeat.rs#L25
std::string ArmHyperVHeartbeat::report() const {
    const char *name = state == State::Closed        ? "closed"
                       : state == State::SendVersion ? "send-version"
                       : state == State::WaitVersion ? "wait-version"
                       : state == State::Ready       ? "ready"
                       : state == State::Failed      ? "failed"
                       : unresponsive                ? "unresponsive"
                                                     : "wait-heartbeat";
    std::ostringstream s;
    s << "{\"state\":\"" << name << "\",\"responsive\":" << (responsive ? "true" : "false")
      << ",\"frameworkMajor\":" << framework << ",\"heartbeatMajor\":" << version
      << ",\"applicationState\":" << applicationState << ",\"negotiations\":" << negotiations
      << ",\"probes\":" << probes << ",\"responses\":" << responses
      << ",\"rejectedResponses\":" << rejected << ",\"timeouts\":" << timeouts << '}';
    return s.str();
}
} // namespace pc
