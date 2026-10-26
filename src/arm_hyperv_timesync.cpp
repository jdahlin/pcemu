// Time synchronization offers the guest a paired host wall-clock and guest
// reference-clock sample. The wall time is UTC FILETIME: 100 ns ticks since
// 1601-01-01. The monotonic VM reference time uses the same units but a different
// origin; pairing lets the guest compensate for delivery delay without treating
// reference time as a calendar date. The caller supplies both sampled clocks.
//
// This integration component (IC) uses a VMBus message-pipe channel to negotiate
// framework 1.0/3.0 and service 4.0.
// The first transactional message carries SYNC, subsequent five-second samples
// carry SAMPLE. The guest owns its clock-adjustment policy; an acknowledged
// packet confirms protocol processing, not a measured correction or accuracy.
// ArmVmbus owns rings/notifications; call packet_published only after publication.
//
// IMPLEMENTATION NOTE: All guest virtual processors must be stopped during calls.
// Ring-full retries rebuild the UTC/reference pair; never retain stale unpublished
// samples. A thirty-second negotiation/sample timeout fails until channel reopen.
// Transaction IDs increment after validated responses; malformed/stale replies
// leave the pending operation unchanged. Host UTC may move backward independently
// of monotonic scheduling. Leap indicator and network time stratum are left zero.
// Microsoft OpenVMM primary protocol, pinned revision: TimesyncMessageV4,
// TimesyncFlags and the 1601 UTC epoch. Shared framing is in arm_hyperv_ic_protocol.hpp.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L28
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54

#include "arm_hyperv_timesync.hpp"
#include <sstream>
#include <stdexcept>
namespace pc {
namespace {
using namespace hyperv_ic;
// TimesyncMessageV4 pairs UTC FILETIME with guest VM reference time; both use
// 100 ns units. SYNC asks for initial synchronization; SAMPLE offers later data.
// Leap indicator/stratum/reserved tail remain zero in this host implementation.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54
struct TimeSyncField {
    enum : size_t { ParentTime = 0, VmReferenceTime = 8, Flags = 16 };
};
constexpr uint8_t SyncTime = 1, SampleTime = 2;
constexpr uint16_t TimeSyncVersion = 4;
constexpr unsigned TimeSyncBodyBytes = 24,
                   NegotiateBodyBytes = NegotiateHeaderBytes + 3 * VersionBytes;
// Number of 100 ns ticks between 1601-01-01 FILETIME and 1970-01-01 Unix UTC.
// This is an epoch conversion, not a guest-clock offset or host timezone.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L28
constexpr int64_t UnixEpochFiletime100ns = 116444736000000000ll;
// GUID wire bytes use little-endian first three GUID components, then literal
// final eight bytes. Service/instance identities are distinct from host channel IDs.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L15
constexpr std::array<uint8_t, 16> TimeSyncInterfaceId{
    0x30, 0xe6, 0x27, 0x95, 0xae, 0xd0, 0x7b, 0x49, 0xad, 0xce, 0xe8, 0x0a, 0xb0, 0x17, 0x5c, 0xaf};
constexpr std::array<uint8_t, 16> TimeSyncInstanceId{
    0x17, 0xce, 0xd1, 0x2d, 0x9e, 0x07, 0x3c, 0x40, 0xb3, 0x52, 0xa1, 0x92, 0x1e, 0xe2, 0x07, 0xee};
} // namespace
// Convert signed Unix ticks to unsigned FILETIME without signed overflow; reject pre-1601 UTC.
// Microsoft OpenVMM TimesyncMessageV4 / TimesyncFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L28
uint64_t ArmHyperVTimeSync::unix_100ns_to_filetime(int64_t ticks) {
    if (ticks < -UnixEpochFiletime100ns)
        throw std::invalid_argument("UTC precedes the FILETIME epoch");
    return ticks < 0 ? uint64_t(UnixEpochFiletime100ns) - uint64_t(-ticks)
                     : uint64_t(UnixEpochFiletime100ns) + uint64_t(ticks);
}
// Build the time-sync message-pipe offer with the OpenVMM service/instance GUIDs.
// Microsoft OpenVMM TimesyncMessageV4 / TimesyncFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L15
ArmVmbus::Offer ArmHyperVTimeSync::channel_offer() {
    ArmVmbus::Offer o{};
    o.channel = ChannelId;
    o.connection = ConnectionId;
    o.flags = IntegrationChannelFlags;
    o.userData[0] = MessagePipeType;
    o.interfaceId = TimeSyncInterfaceId;
    o.instanceId = TimeSyncInstanceId;
    return o;
}
// Start framework negotiation and reset transaction/initial-sync state; preserve host counters.
// Microsoft OpenVMM TimesyncMessageV4 / TimesyncFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54
void ArmHyperVTimeSync::channel_opened(uint64_t now) {
    state = State::SendVersion;
    framework = 0;
    transaction = 0;
    initial = true;
    since = now;
}
// Stop the session and clear the framework, preventing further queued samples.
// Microsoft OpenVMM TimesyncMessageV4 / TimesyncFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54
void ArmHyperVTimeSync::channel_closed() {
    state = State::Closed;
    framework = 0;
}
// Build a negotiation or fresh UTC/reference sample; expiration fails the session until reopen.
// IMPLEMENTATION NOTE: Rebuild unpublished samples on every retry so ring backpressure
// cannot make an old paired clock reading appear freshly measured.
// Microsoft OpenVMM TimesyncMessageV4 / TimesyncFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54
std::vector<uint8_t> ArmHyperVTimeSync::pending_packet(uint64_t now, uint64_t utc) {
    if ((state == State::WaitVersion || state == State::WaitSample) &&
        now - since >= ResponseTimeout100ns) {
        state = State::Failed;
        ++timeouts;
    }
    bool negotiate = state == State::SendVersion;
    if (!negotiate && !(state == State::Ready && (initial || now - since >= SampleInterval100ns)))
        return {};
    unsigned body = negotiate ? NegotiateBodyBytes : TimeSyncBodyBytes;
    std::vector<uint8_t> p((BodyOffset + body + PacketAlignmentMask) & ~PacketAlignmentMask);
    write_little_endian(p, PacketField::Type, InBandPacketType, 2);
    write_little_endian(p, PacketField::DataOffset8, InBandDataOffset8, 2);
    write_little_endian(p, PacketField::Length8, p.size() / PacketUnitBytes, 2);
    write_little_endian(p, WireField::PipeType, PipeDataPacketType, 4);
    write_little_endian(p, WireField::PipeMessageBytes, HeaderBytes + body, 4);
    write_little_endian(p, WireField::FrameworkMajor, negotiate ? 0 : framework, 2);
    write_little_endian(p, WireField::MessageType,
                        negotiate ? MessageType::Negotiate : MessageType::TimeSync, 2);
    write_little_endian(p, WireField::MessageMajor, negotiate ? 0 : TimeSyncVersion, 2);
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
        write_little_endian(p, BodyOffset + NegotiateField::OfferedService1Major, TimeSyncVersion,
                            2);
    } else {
        write_little_endian(p, BodyOffset + TimeSyncField::ParentTime, utc, 8);
        write_little_endian(p, BodyOffset + TimeSyncField::VmReferenceTime, now, 8);
        p[BodyOffset + TimeSyncField::Flags] = initial ? SyncTime : SampleTime;
        // NTP leap indicator and stratum remain unspecified (zero).
    }
    return p;
}
// Start a negotiation/sample reply deadline and count only successful ring publication.
// Microsoft OpenVMM TimesyncMessageV4 / TimesyncFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54
void ArmHyperVTimeSync::packet_published(uint64_t now) {
    if (state == State::SendVersion) {
        state = State::WaitVersion;
        ++negotiations;
    } else if (state == State::Ready && (initial || now - since >= SampleInterval100ns)) {
        state = State::WaitSample;
        ++samples;
    } else
        return;
    since = now;
}
// Validate framing, response flags/status, current transaction and negotiated framework/service.
// Select service 4.0 only; successful acknowledgment schedules the next sample, not proof
// that the guest applied the supplied wall time.
// Microsoft OpenVMM TimesyncMessageV4 / TimesyncFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54
void ArmHyperVTimeSync::receive_response(std::span<const uint8_t> p, uint64_t now) {
    auto fail = [&] { ++rejected; };
    if ((state == State::WaitVersion || state == State::WaitSample) &&
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
    auto bodySize = read_little_endian(h, HeaderField::BodyBytes, 2);
    if (bodySize > h.size() - HeaderBytes || read_little_endian(h, HeaderField::Status, 4) ||
        h[HeaderField::TransactionId] != transaction ||
        h[HeaderField::Flags] != TransactionResponse)
        return fail();
    auto body = h.subspan(HeaderBytes, size_t(bodySize));
    if (state == State::WaitVersion &&
        read_little_endian(h, HeaderField::MessageType, 2) == MessageType::Negotiate) {
        if (body.size() < SelectedVersionsBytes ||
            read_little_endian(body, NegotiateField::FrameworkCount, 2) != 1 ||
            read_little_endian(body, NegotiateField::ServiceCount, 2) != 1 ||
            read_little_endian(body, NegotiateField::FrameworkMinor, 2) ||
            read_little_endian(body, NegotiateField::ServiceMajor, 2) != TimeSyncVersion ||
            read_little_endian(body, NegotiateField::ServiceMinor, 2))
            return fail();
        auto fw = read_little_endian(body, NegotiateField::FrameworkMajor, 2);
        if (fw != FrameworkVersion1 && fw != FrameworkVersion3)
            return fail();
        framework = uint16_t(fw);
        state = State::Ready;
        ++transaction;
        since = now;
        return;
    }
    if (state != State::WaitSample || now - since >= ResponseTimeout100ns ||
        read_little_endian(h, HeaderField::MessageType, 2) != MessageType::TimeSync ||
        read_little_endian(h, HeaderField::FrameworkMajor, 2) != framework ||
        read_little_endian(h, HeaderField::FrameworkMinor, 2) ||
        read_little_endian(h, HeaderField::MessageMajor, 2) != TimeSyncVersion ||
        read_little_endian(h, HeaderField::MessageMinor, 2))
        return fail();
    ++acks;
    ++transaction;
    initial = false;
    since = now;
    state = State::Ready;
}
// Return ticks until initial sample, periodic sample or response deadline; none when closed/failed.
// IMPLEMENTATION NOTE: Unsigned elapsed subtraction handles reference-counter wrap.
// Microsoft OpenVMM TimesyncMessageV4 / TimesyncFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54
std::optional<uint64_t> ArmHyperVTimeSync::next_delay_100ns(uint64_t now) const {
    if (state == State::SendVersion || (state == State::Ready && initial))
        return 0;
    if (state == State::Ready)
        return now - since >= SampleInterval100ns ? 0 : SampleInterval100ns - (now - since);
    if (state == State::WaitVersion || state == State::WaitSample)
        return now - since >= ResponseTimeout100ns ? 0 : ResponseTimeout100ns - (now - since);
    return {};
}
// Report bounded version/sample/acknowledgment/rejection counters without exposing timestamps.
// Microsoft OpenVMM TimesyncMessageV4 / TimesyncFlags.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/timesync.rs#L54
std::string ArmHyperVTimeSync::report() const {
    const char *name = state == State::Closed        ? "closed"
                       : state == State::SendVersion ? "send-version"
                       : state == State::WaitVersion ? "wait-version"
                       : state == State::Ready       ? "ready"
                       : state == State::WaitSample  ? "wait-sample"
                                                     : "failed";
    std::ostringstream s;
    s << "{\"state\":\"" << name << "\",\"frameworkMajor\":" << framework
      << ",\"timesyncMajor\":" << (framework ? TimeSyncVersion : 0)
      << ",\"negotiations\":" << negotiations << ",\"samples\":" << samples
      << ",\"acknowledgments\":" << acks << ",\"rejectedResponses\":" << rejected
      << ",\"timeouts\":" << timeouts << "}";
    return s.str();
}
} // namespace pc
