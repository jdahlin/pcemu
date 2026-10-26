#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
namespace pc::hyperv_ic {
// Integration Component (IC) framing shared by heartbeat, time sync, shutdown
// and key/value exchange. Offsets are byte offsets within the named structure,
// never device register addresses. The complete ring packet contains a VMBus
// descriptor, message-pipe header, IC header, body, then zero padding to eight bytes.
// Microsoft's OpenVMM IC Header/NegotiateMessage/HeaderFlags (pinned revision):
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L89
// Message-pipe framing and VMBus descriptor:
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_ring/src/lib.rs#L46
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h#L357
constexpr size_t PacketDescriptorBytes = 16, PacketUnitBytes = 8,
                 PacketAlignmentMask = PacketUnitBytes - 1, PipeHeaderBytes = 8, HeaderBytes = 20,
                 HeaderOffset = PacketDescriptorBytes + PipeHeaderBytes,
                 BodyOffset = HeaderOffset + HeaderBytes;
// In-band packets have no separate completion request. offset8/len8 count units
// of eight bytes including the descriptor; pipe length excludes its own header.
constexpr uint16_t InBandPacketType = 6,
                   InBandDataOffset8 = PacketDescriptorBytes / PacketUnitBytes;
constexpr uint32_t PipeDataPacketType = 1;
struct PacketField {
    enum : size_t { Type = 0, DataOffset8 = 2, Length8 = 4 };
};
struct PipeField {
    enum : size_t { Type = 0, MessageBytes = 4 };
};
// Versions are major/minor u16 pairs. Size counts only body bytes; status is a
// response code, transaction correlates requests/replies, trailing two bytes reserved.
struct HeaderField {
    enum : size_t {
        FrameworkMajor = 0,
        FrameworkMinor = 2,
        MessageType = 4,
        MessageMajor = 6,
        MessageMinor = 8,
        BodyBytes = 10,
        Status = 12,
        TransactionId = 16,
        Flags = 17,
        Reserved = 18,
    };
};
// Complete-packet offsets for writing an outgoing IC header.
struct WireField {
    enum : size_t {
        PipeType = PacketDescriptorBytes + PipeField::Type,
        PipeMessageBytes = PacketDescriptorBytes + PipeField::MessageBytes,
        FrameworkMajor = HeaderOffset + HeaderField::FrameworkMajor,
        FrameworkMinor = HeaderOffset + HeaderField::FrameworkMinor,
        MessageType = HeaderOffset + HeaderField::MessageType,
        MessageMajor = HeaderOffset + HeaderField::MessageMajor,
        MessageMinor = HeaderOffset + HeaderField::MessageMinor,
        BodyBytes = HeaderOffset + HeaderField::BodyBytes,
        Status = HeaderOffset + HeaderField::Status,
        TransactionId = HeaderOffset + HeaderField::TransactionId,
        Flags = HeaderOffset + HeaderField::Flags,
    };
};
// IC operation types, independent of VMBus control-message and packet type enums.
struct MessageType {
    enum : uint16_t { Negotiate = 0, Heartbeat = 1, KeyValuePair = 2, Shutdown = 3, TimeSync = 4 };
};
// Header flags bit 0 transaction, bit 1 request, bit 2 response. Thus 03h is a
// transactional request and 05h its transactional response; zero status succeeds.
constexpr uint8_t Transaction = 1, Request = 2, Response = 4,
                  TransactionRequest = Transaction | Request,
                  TransactionResponse = Transaction | Response;
// Frameworks offered by these host services are 1.0 and 3.0. Negotiation body
// begins with two u16 version counts and reserved u32, followed by version pairs.
// A selected response has one framework and one service version at offsets 8/12.
constexpr uint16_t FrameworkVersion1 = 1, FrameworkVersion3 = 3;
constexpr size_t VersionBytes = 4, NegotiateHeaderBytes = 8,
                 SelectedVersionsBytes = NegotiateHeaderBytes + 2 * VersionBytes;
struct NegotiateField {
    enum : size_t {
        FrameworkCount = 0,
        ServiceCount = 2,
        Reserved = 4,
        FrameworkMajor = 8,
        FrameworkMinor = 10,
        ServiceMajor = 12,
        ServiceMinor = 14,
        OfferedFramework1Major = 8,
        OfferedFramework3Major = 12,
        OfferedService1Major = 16,
        OfferedService1Minor = 18,
        OfferedService2Major = 20,
        OfferedService3Major = 24,
    };
};
// Channel offer: enumerate interface (bit 0), named-pipe mode (bit 4), message
// pipe type 4 in the low u32 of user data. This is not pipe DATA packet type 1.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/vmbus/vmbus_core/src/protocol.rs#L550
constexpr uint16_t IntegrationChannelFlags = 0x11;
constexpr uint8_t MessagePipeType = 4;
// Reference time/deadlines use 100 ns ticks (10,000,000 per second), not CPU cycles.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-time-enlightenment
constexpr uint64_t TicksPerSecond100ns = 10000000;
// Decode a little-endian wire field after the caller validates its complete bounds.
// OpenVMM IC Header and version-pair layout.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L89
inline uint64_t read_little_endian(std::span<const uint8_t> bytes, size_t at, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= uint64_t(bytes[at + i]) << (8 * i);
    return value;
}
// Encode a little-endian field into caller-sized storage; reserved bytes stay zero.
// OpenVMM IC Header and version-pair layout.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs#L89
inline void write_little_endian(std::span<uint8_t> bytes, size_t at, uint64_t value,
                                unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        bytes[at + i] = uint8_t(value >> (8 * i));
}
} // namespace pc::hyperv_ic
