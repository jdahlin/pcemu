// The Key-Value Pair (KVP) Integration Component (IC) transports explicit host
// queries/mutations to a guest-maintained collection of named values. Opening its
// VMBus message-pipe channel negotiates framework 1.0/3.0 and KVP 3.0/4.0/5.0;
// it never enumerates or changes guest data automatically. Pool selectors choose
// separate guest collections, and the payload codec handles their standard typed
// UTF-16 key/value records. IP-address configuration messages are unsupported.
//
// The host queues one GET/SET/DELETE/ENUMERATE after negotiation. ArmVmbus owns
// ring publication/notifications; packet_published starts the response deadline.
// A response must match transaction, selected versions, operation and key/index.
// Successful reads retain a decoded record; mutations retain an acknowledgment;
// guest error statuses are retained without fabricating values. The host must
// take_result before issuing another operation. Enumeration stops explicitly at
// NO_MORE_ITEMS; no implicit follow-up query or retry is generated here.
//
// IMPLEMENTATION NOTE: All guest virtual processors must be stopped during calls.
// Host mutations are limited to External/AutoExternal pools; pool contents and
// persistence remain guest-service responsibilities. A thirty-second published
// request timeout fails until reopen. Closing discards query/unread result state,
// while lifetime counters remain. Diagnostics expose only bounded protocol fields,
// never guest keys/values. The codec validates payloads separately from this file's
// IC framing/transaction/status checks. Shared wire fields are in
// arm_hyperv_ic_protocol.hpp; primary KVP definitions are Microsoft's OpenVMM,
// pinned to revision 76420ea20a67ef65c80a62f1fc93de3b8f1eabaa.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L20
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L89

#include "arm_hyperv_kvp.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>
namespace pc {
namespace {
using namespace hyperv_ic;
// Service versions 3.0, 4.0 and 5.0 share the standard KVP body used here; IP
// configuration operations remain unsupported even if the selected version allows them.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L20
constexpr uint16_t FirstKvpVersion = 3, LastKvpVersion = 5;
constexpr unsigned NegotiateBodyBytes = NegotiateHeaderBytes + 5 * VersionBytes;
// 80070103h is IC NO_MORE_ITEMS (HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)), a
// normal enumeration end rather than a malformed protocol response.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L114
constexpr uint32_t NoMoreItems = 0x80070103;
// Distinct service/instance GUIDs in wire byte order (first three components LE).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L15
constexpr std::array<uint8_t, 16> KvpInterfaceId{0xe7, 0xf4, 0xa0, 0xa9, 0x45, 0x5a, 0x96, 0x4d,
                                                 0xb8, 0x27, 0x8a, 0x84, 0x1e, 0x8c, 0x03, 0xe6};
constexpr std::array<uint8_t, 16> KvpInstanceId{0x19, 0xf9, 0x2f, 0x24, 0xdb, 0x07, 0x80, 0x41,
                                                0x9c, 0x2e, 0xb8, 0x6c, 0xb6, 0x8c, 0x8c, 0x55};
} // namespace
// Build a real KVP named-pipe offer; GUIDs identify service, IDs select host transport routes.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L15
ArmVmbus::Offer ArmHyperVKvp::channel_offer() {
    ArmVmbus::Offer o{};
    o.channel = ChannelId;
    o.connection = ConnectionId;
    o.flags = IntegrationChannelFlags;
    o.userData[0] = MessagePipeType;
    o.interfaceId = KvpInterfaceId;
    o.instanceId = KvpInstanceId;
    return o;
}
// Begin negotiation and clear query/result/last-response metadata; keep lifetime counters.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
void ArmHyperVKvp::channel_opened(uint64_t now) {
    state = State::SendVersion;
    framework = 0;
    service = 0;
    transaction = 0;
    since = now;
    result.reset();
    queryBody.clear();
    queryKey.clear();
    lastStatus = 0;
    versionReserved = versionBodySize = versionFrameworkCount = versionServiceCount =
        versionFramework = versionService = 0;
}
// Stop session work, clear versions and discard a queued query or unread result.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
void ArmHyperVKvp::channel_closed() {
    state = State::Closed;
    framework = 0;
    service = 0;
    result.reset();
    queryBody.clear();
    queryKey.clear();
}
// Queue one explicit operation only when Ready, result-free and valid for the selected pool.
// IMPLEMENTATION NOTE: Allow host mutations only in External/AutoExternal; encode/validate
// the complete body before committing pending state. A bad request changes nothing.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L38
bool ArmHyperVKvp::queue_request(ArmHyperVKvpCodec::Operation op, ArmHyperVKvpCodec::Pool requested,
                                 uint32_t at, const std::u16string &key,
                                 const ArmHyperVKvpCodec::Value *value) {
    if (state != State::Ready || result ||
        unsigned(requested) > unsigned(ArmHyperVKvpCodec::Pool::AutoExternal))
        return false;
    if ((op == ArmHyperVKvpCodec::Operation::Set || op == ArmHyperVKvpCodec::Operation::Delete) &&
        requested != ArmHyperVKvpCodec::Pool::External &&
        requested != ArmHyperVKvpCodec::Pool::AutoExternal)
        return false;
    std::vector<uint8_t> body;
    try {
        body = ArmHyperVKvpCodec::encode_request(op, requested, at, key, value);
    } catch (const std::invalid_argument &) {
        return false;
    }
    queryBody = std::move(body);
    queryKey = key;
    operation = op;
    pool = requested;
    index = at;
    state = State::SendQuery;
    return true;
}
// Queue one explicit enumeration index; no automatic walk of a guest pool is started.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L136
bool ArmHyperVKvp::enumerate_value(ArmHyperVKvpCodec::Pool p, uint32_t i) {
    return queue_request(ArmHyperVKvpCodec::Operation::Enumerate, p, i);
}
// Queue one explicit exact-key read; operation results remain owned by the host until taken.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L113
bool ArmHyperVKvp::get_value(ArmHyperVKvpCodec::Pool p, const std::u16string &key) {
    return queue_request(ArmHyperVKvpCodec::Operation::Get, p, 0, key);
}
// Queue one explicit typed value mutation in a host-writable pool; encoding does not publish it.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L113
bool ArmHyperVKvp::set_value(ArmHyperVKvpCodec::Pool p, const std::u16string &key,
                             const ArmHyperVKvpCodec::Value &v) {
    return queue_request(ArmHyperVKvpCodec::Operation::Set, p, 0, key, &v);
}
// Queue one explicit key deletion in a host-writable pool; encoding does not publish it.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L121
bool ArmHyperVKvp::delete_value(ArmHyperVKvpCodec::Pool p, const std::u16string &key) {
    return queue_request(ArmHyperVKvpCodec::Operation::Delete, p, 0, key);
}
// Move the one retained decoded result out and clear the unread-result gate.
// IMPLEMENTATION NOTE: Host result ownership is separate from wire publication/acknowledgment.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
std::optional<ArmHyperVKvp::Result> ArmHyperVKvp::take_result() {
    return std::exchange(result, {});
}
// Fail a negotiation/query session after thirty seconds awaiting its published request.
// IMPLEMENTATION NOTE: No automatic operation retry; reopen is required after failure.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
void ArmHyperVKvp::expire_response(uint64_t now) {
    if ((state == State::WaitVersion || state == State::WaitResponse) &&
        now - since >= ResponseTimeout100ns) {
        state = State::Failed;
        ++timeouts;
    }
}
// Build a due version/query packet without starting its response deadline or advancing transaction.
// Negotiation offers two frameworks and three KVP versions; query payload stays stable under
// backpressure. Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L20
std::vector<uint8_t> ArmHyperVKvp::pending_packet(uint64_t now) {
    expire_response(now);
    bool negotiation = state == State::SendVersion;
    if (!negotiation && state != State::SendQuery)
        return {};
    auto body = negotiation ? std::vector<uint8_t>(NegotiateBodyBytes) : queryBody;
    if (negotiation) {
        write_little_endian(body, NegotiateField::FrameworkCount, 2, 2);
        write_little_endian(body, NegotiateField::ServiceCount, 3, 2);
        write_little_endian(body, NegotiateField::OfferedFramework1Major, FrameworkVersion1, 2);
        write_little_endian(body, NegotiateField::OfferedFramework3Major, FrameworkVersion3, 2);
        write_little_endian(body, NegotiateField::OfferedService1Major, FirstKvpVersion, 2);
        write_little_endian(body, NegotiateField::OfferedService2Major, 4, 2);
        write_little_endian(body, NegotiateField::OfferedService3Major, LastKvpVersion, 2);
    }
    std::vector<uint8_t> p((BodyOffset + body.size() + PacketAlignmentMask) & ~PacketAlignmentMask);
    write_little_endian(p, PacketField::Type, InBandPacketType, 2);
    write_little_endian(p, PacketField::DataOffset8, InBandDataOffset8, 2);
    write_little_endian(p, PacketField::Length8, p.size() / PacketUnitBytes, 2);
    write_little_endian(p, WireField::PipeType, PipeDataPacketType, 4);
    write_little_endian(p, WireField::PipeMessageBytes, HeaderBytes + body.size(), 4);
    write_little_endian(p, WireField::FrameworkMajor, negotiation ? 0 : framework, 2);
    write_little_endian(p, WireField::MessageType,
                        negotiation ? MessageType::Negotiate : MessageType::KeyValuePair, 2);
    write_little_endian(p, WireField::MessageMajor, negotiation ? 0 : service, 2);
    write_little_endian(p, WireField::BodyBytes, body.size(), 2);
    p[WireField::TransactionId] = transaction;
    p[WireField::Flags] = TransactionRequest;
    std::copy(body.begin(), body.end(), p.begin() + BodyOffset);
    return p;
}
// Count a successfully published version/query and start its response deadline.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
void ArmHyperVKvp::packet_published(uint64_t now) {
    if (state == State::SendVersion) {
        state = State::WaitVersion;
        ++negotiations;
    } else if (state == State::SendQuery) {
        state = State::WaitResponse;
        ++requests;
    } else
        return;
    since = now;
}
// Validate packet/pipe/IC bounds, transaction, flags, reserved fields and selected versions.
// On success decode the requested record or mutation identity; retain one result and advance
// transaction. NO_MORE_ITEMS terminates enumeration; other guest statuses are returned unchanged.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
void ArmHyperVKvp::receive_response(std::span<const uint8_t> p, uint64_t now) {
    expire_response(now);
    auto fail = [&] { ++rejected; };
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
    auto length = read_little_endian(h, HeaderField::BodyBytes, 2);
    if (length > h.size() - HeaderBytes || h[HeaderField::TransactionId] != transaction ||
        h[HeaderField::Flags] != TransactionResponse ||
        read_little_endian(h, HeaderField::Reserved, 2))
        return fail();
    auto body = h.subspan(HeaderBytes, size_t(length));
    auto status = uint32_t(read_little_endian(h, HeaderField::Status, 4));
    if (state == State::WaitVersion &&
        read_little_endian(h, HeaderField::MessageType, 2) == MessageType::Negotiate) {
        versionBodySize = uint16_t(body.size());
        if (body.size() >= NegotiateHeaderBytes) {
            versionFrameworkCount =
                uint16_t(read_little_endian(body, NegotiateField::FrameworkCount, 2));
            versionServiceCount =
                uint16_t(read_little_endian(body, NegotiateField::ServiceCount, 2));
            versionReserved = uint32_t(read_little_endian(body, NegotiateField::Reserved, 4));
        }
        if (body.size() >= SelectedVersionsBytes) {
            versionFramework =
                uint32_t(read_little_endian(body, NegotiateField::FrameworkMajor, 4));
            versionService = uint32_t(read_little_endian(body, NegotiateField::ServiceMajor, 4));
        }
        if (status) {
            state = State::Failed;
            lastStatus = status;
            return;
        }
        if (body.size() < SelectedVersionsBytes ||
            read_little_endian(body, NegotiateField::FrameworkCount, 2) != 1 ||
            read_little_endian(body, NegotiateField::ServiceCount, 2) != 1 ||
            read_little_endian(body, NegotiateField::Reserved, 4) ||
            read_little_endian(body, NegotiateField::FrameworkMinor, 2) ||
            (read_little_endian(body, NegotiateField::ServiceMajor, 2) < FirstKvpVersion ||
             read_little_endian(body, NegotiateField::ServiceMajor, 2) > LastKvpVersion) ||
            read_little_endian(body, NegotiateField::ServiceMinor, 2))
            return fail();
        auto fw = read_little_endian(body, NegotiateField::FrameworkMajor, 2);
        if (fw != FrameworkVersion1 && fw != FrameworkVersion3)
            return fail();
        framework = uint16_t(fw);
        service = uint16_t(read_little_endian(body, NegotiateField::ServiceMajor, 2));
        state = State::Ready;
        ++transaction;
        return;
    }
    if (state != State::WaitResponse ||
        read_little_endian(h, HeaderField::FrameworkMajor, 2) != framework ||
        read_little_endian(h, HeaderField::FrameworkMinor, 2) ||
        read_little_endian(h, HeaderField::MessageType, 2) != MessageType::KeyValuePair ||
        read_little_endian(h, HeaderField::MessageMajor, 2) != service ||
        read_little_endian(h, HeaderField::MessageMinor, 2))
        return fail();
    std::optional<ArmHyperVKvpCodec::Record> record;
    if (!status) {
        if (operation == ArmHyperVKvpCodec::Operation::Enumerate)
            record = ArmHyperVKvpCodec::decode_enumeration(body, pool, index);
        else if (operation == ArmHyperVKvpCodec::Operation::Get)
            record = ArmHyperVKvpCodec::decode_get_response(body, pool, queryKey);
        else {
            if (!ArmHyperVKvpCodec::mutation_acknowledged(body, operation, pool, queryKey))
                return fail();
            ++mutationAcks;
        }
        if (operation == ArmHyperVKvpCodec::Operation::Enumerate ||
            operation == ArmHyperVKvpCodec::Operation::Get) {
            if (!record)
                return fail();
            ++records;
        }
    } else if (status == NoMoreItems && operation == ArmHyperVKvpCodec::Operation::Enumerate)
        ++ends;
    result = Result{pool, index, status, std::move(record), operation};
    lastStatus = status;
    queryBody.clear();
    queryKey.clear();
    ++responses;
    ++transaction;
    state = State::Ready;
}
// Return remaining host ticks until due work/response expiration; none when idle or failed.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
std::optional<uint64_t> ArmHyperVKvp::next_delay_100ns(uint64_t now) const {
    if (state == State::SendVersion || state == State::SendQuery)
        return 0;
    if (state == State::WaitVersion || state == State::WaitResponse)
        return now - since >= ResponseTimeout100ns ? 0 : ResponseTimeout100ns - (now - since);
    return {};
}
// Report bounded state/counts and selected negotiation fields; exclude guest keys/value content.
// Version diagnostics retain packed major/minor pairs only after declared-body bounds checks.
// Microsoft OpenVMM KVP / IC negotiation wire definitions.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L20
std::string ArmHyperVKvp::report() const {
    const char *name = state == State::Closed         ? "closed"
                       : state == State::SendVersion  ? "send-version"
                       : state == State::WaitVersion  ? "wait-version"
                       : state == State::Ready        ? "ready"
                       : state == State::SendQuery    ? "send-query"
                       : state == State::WaitResponse ? "wait-response"
                                                      : "failed";
    std::ostringstream s;
    s << "{\"state\":\"" << name << "\",\"frameworkMajor\":" << framework
      << ",\"kvpMajor\":" << service << ",\"negotiations\":" << negotiations
      << ",\"requests\":" << requests << ",\"responses\":" << responses
      << ",\"records\":" << records << ",\"versionReply\":{\"bodyBytes\":" << versionBodySize
      << ",\"frameworkCount\":" << versionFrameworkCount
      << ",\"serviceCount\":" << versionServiceCount << ",\"reserved\":" << versionReserved
      << ",\"framework\":" << versionFramework << ",\"service\":" << versionService << '}'
      << ",\"operation\":" << unsigned(operation) << ",\"mutationAcknowledgments\":" << mutationAcks
      << ",\"endOfPool\":" << ends << ",\"lastStatus\":" << lastStatus
      << ",\"resultPending\":" << (result ? "true" : "false")
      << ",\"rejectedResponses\":" << rejected << ",\"timeouts\":" << timeouts << '}';
    return s.str();
}
} // namespace pc
