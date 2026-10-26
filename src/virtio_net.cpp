// Virtio network supplies Ethernet frames through one receive/transmit queue pair.
// The guest posts writable receive buffers and readable transmit buffers, each
// prefixed by a modern 12-byte virtio_net_hdr. This model offers a fixed MAC,
// always-up link status, MTU and transmit checksum completion. It accepts ordinary
// unsegmented frames; receive headers are zero because no receive offload or
// mergeable buffers are offered. The machine/network backend consumes the host
// outgoing deque and injects complete received frames through receive().
//
// Split-ring completion publishes used lengths/indices before raising the callback.
// EVENT_IDX, when negotiated, uses wrapping index comparisons rather than the
// older NO_INTERRUPT flag. All DMA and backend exchange occur with CPUs stopped.
// IMPLEMENTATION NOTE: RAM bounds, 256-entry queues, 64 KiB chain assembly and
// 256 outgoing frames bound host work/storage. Receive frames are dropped if no
// buffer is available; a full outgoing backlog drops frames while completing TX.
// No segmentation, control virtqueue, multiqueue, indirect or packed rings exist.
// COMPATIBILITY: QueueReady=0 clears indices without RING_RESET negotiation;
// status writes replace flags. Reset retains host counters and clears outgoing.
// FIXME: Enforce the modern initialization/queue-reset rules before claiming full
// transport conformance. Preserve current behavior in this documentation migration.
// Virtio 1.2, 5.1 Network Device, 2.7 Split Virtqueues, 4.2.2 MMIO.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002

#include "virtio_net.hpp"
#include "virtio_registers.hpp"
#include <algorithm>
#include <bitset>
#include <cstring>
#include <stdexcept>
namespace pc {
namespace {
// Virtio 1.2, 5.1: network device and the sole receive/transmit queue pair.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
constexpr uint32_t NetworkDeviceId = 1;
enum QueueIndex : unsigned { ReceiveQueue = 0, TransmitQueue = 1 };
constexpr unsigned QueueCount = 2;
// IMPLEMENTATION NOTE: Bound packet assembly and the queued host transmit backlog.
constexpr unsigned MaximumChainBytes = 65536, MaximumOutgoingFrames = 256;
// IMPLEMENTATION NOTE: Ethernet frame acceptance bounds retained by this backend.
// These count frame bytes supplied by the backend, excluding the Virtio header.
constexpr unsigned MinimumFrameBytes = 14, MaximumFrameBytes = 1518;
// Virtio 1.2, 5.1.6 virtio_net_hdr: modern non-legacy header includes num_buffers.
// The device offers no segmentation/mergeable receive buffers; GSO type must be zero.
enum PacketHeaderField : unsigned {
    Flags = 0,            // Bit 0 NEEDS_CSUM requests transmit checksum completion.
    SegmentationType = 1, // gso_type=0 means no segmentation offload.
    ChecksumStart = 6,    // Little-endian u16 offset from the Ethernet frame start.
    ChecksumOffset = 8,   // Little-endian u16 checksum field offset relative to start.
};
constexpr unsigned PacketHeaderBytes = 12;
constexpr uint8_t NeedsChecksum = 1;
// One's-complement Internet checksum operates on 16-bit big-endian words.
// https://www.rfc-editor.org/rfc/rfc1071.html#section-1
constexpr unsigned ChecksumWordBytes = 2, ChecksumWordBits = 16;
constexpr uint32_t ChecksumWordMask = 0xffff;
// Device configuration: mac[6], status:u16, max_virtqueue_pairs:u16, mtu:u16.
// Only MAC, STATUS and MTU are offered; maximum queue-pair field remains zero.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
enum ConfigurationField : unsigned { MacAddress = 0, LinkStatus = 6, MaximumTransmissionUnit = 10 };
// IMPLEMENTATION NOTE: Fixed board MAC retained for existing guests/backends;
// it is not a Virtio-assigned address and must be unique if boards share a LAN.
constexpr std::array<uint8_t, 6> BoardMacAddress{0x52, 0x54, 0, 0x12, 0x34, 0x56};
constexpr uint16_t LinkUp = 1; // VIRTIO_NET_S_LINK_UP, status bit 0.
constexpr uint16_t FramePayloadMtu =
    1500; // Advertised IP/Ethernet payload limit, not header bytes.
// IMPLEMENTATION NOTE: Synthesize only the first 64 device configuration bytes.
constexpr unsigned DeviceConfigurationBytes = 64, DeviceConfigurationEnd = 0x140;
// Transport feature/address words use 32-bit halves; byte lanes are little-endian.
constexpr unsigned FeatureWordBits = 32, FeatureWordCount = 2, BitsPerByte = 8;
constexpr uint64_t LowAddressMask = 0xffffffffull, HighAddressMask = 0xffffffff00000000ull;
// Queue address registers repeat at 0x10-byte strides, with low/high words at +0/+4.
constexpr unsigned QueueAddressRegisterStrideMask = 0x0f, HighAddressWordOffset = 4;
constexpr uint32_t InitializedStatus =
    virtio::Acknowledge | virtio::Driver | virtio::FeaturesOk | virtio::DriverOk;
constexpr uint32_t FailureStatus = virtio::DeviceNeedsReset | virtio::Failed;
} // namespace

// Bind guest RAM and interrupt delivery for one receive/transmit queue pair.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
VirtioNet::VirtioNet(std::span<uint8_t> r, uint64_t base, std::function<void(bool)> irq)
    : ram(r), ramBase(base), interrupt(std::move(irq)) {}
// Resolve a bounded guest physical RAM range; reject overflow/outside accesses.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
uint8_t *VirtioNet::memory(uint64_t address, uint64_t length) const {
    if (address < ramBase || length > ram.size() || address - ramBase > ram.size() - length)
        throw std::runtime_error("virtio-net DMA outside guest RAM");
    return ram.data() + (address - ramBase);
}
// Read one little-endian ring field from validated RAM.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
uint64_t VirtioNet::read_guest_value(uint64_t address, unsigned width) const {
    auto *p = memory(address, width);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(p[i]) << (i * BitsPerByte);
    return value;
}
// Write one little-endian ring field in validated RAM.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
void VirtioNet::write_guest_value(uint64_t address, uint64_t value, unsigned width) {
    auto *p = memory(address, width);
    for (unsigned i = 0; i < width; ++i)
        p[i] = value >> (i * BitsPerByte);
}
// Read MMIO state or the fixed MAC/link/MTU device configuration.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
uint64_t VirtioNet::read(unsigned offset, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio-net register read");
    if (offset >= virtio::Register::DeviceConfigurationRegisters &&
        offset < DeviceConfigurationEnd && width <= DeviceConfigurationEnd - offset) {
        std::array<uint8_t, DeviceConfigurationBytes> config{};
        std::copy(BoardMacAddress.begin(), BoardMacAddress.end(),
                  config.begin() + ConfigurationField::MacAddress);
        config[ConfigurationField::LinkStatus] = uint8_t(LinkUp);
        config[ConfigurationField::LinkStatus + 1] = uint8_t(LinkUp >> BitsPerByte);
        config[ConfigurationField::MaximumTransmissionUnit] = uint8_t(FramePayloadMtu);
        config[ConfigurationField::MaximumTransmissionUnit + 1] =
            uint8_t(FramePayloadMtu >> BitsPerByte);
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= uint64_t(config[offset - virtio::Register::DeviceConfigurationRegisters + i])
                     << (8 * i);
        return value;
    }
    if (width != 4)
        throw std::runtime_error("virtio-net control read requires 32 bits");
    const Queue empty{};
    const auto &q = queueSelect < QueueCount ? queues[queueSelect] : empty;
    switch (offset) {
    case virtio::Register::MagicValueRegister:
        return virtio::MmioMagicValue;
    case virtio::Register::VersionRegister:
        return virtio::ModernMmioVersion;
    case virtio::Register::DeviceIdRegister:
        return NetworkDeviceId;
    case virtio::Register::VendorIdRegister:
        return virtio::PcemuVendorId;
    case virtio::Register::DeviceFeaturesRegister:
        return featureSelect < FeatureWordCount
                   ? uint32_t(features >> (FeatureWordBits * featureSelect))
                   : 0;
    case virtio::Register::DeviceFeaturesSelectorRegister:
        return featureSelect;
    case virtio::Register::DriverFeaturesRegister:
        return driverSelect < FeatureWordCount
                   ? uint32_t(driverFeatures >> (FeatureWordBits * driverSelect))
                   : 0;
    case virtio::Register::DriverFeaturesSelectorRegister:
        return driverSelect;
    case virtio::Register::QueueSelectorRegister:
        return queueSelect;
    case virtio::Register::QueueMaximumSizeRegister:
        return queueSelect < QueueCount ? MaximumQueueEntries : 0;
    case virtio::Register::QueueSizeRegister:
        return queueSelect < QueueCount ? q.size : 0;
    case virtio::Register::QueueReadyRegister:
        return q.ready;
    case virtio::Register::InterruptStatusRegister:
        return isr;
    case virtio::Register::StatusRegister:
        return status;
    case virtio::Register::QueueDescriptorLowRegister:
        return uint32_t(q.descriptors);
    case virtio::Register::QueueDescriptorHighRegister:
        return q.descriptors >> FeatureWordBits;
    case virtio::Register::QueueDriverLowRegister:
        return uint32_t(q.available);
    case virtio::Register::QueueDriverHighRegister:
        return q.available >> FeatureWordBits;
    case virtio::Register::QueueDeviceLowRegister:
        return uint32_t(q.used);
    case virtio::Register::QueueDeviceHighRegister:
        return q.used >> FeatureWordBits;
    default:
        return 0;
    }
}
// Record a host queue failure, set DEVICE_NEEDS_RESET and notify configuration change.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
void VirtioNet::require_device_reset(const std::string &reason) {
    failure = reason;
    status |= virtio::DeviceNeedsReset;
    isr |= virtio::ConfigurationChangeInterrupt;
    interrupt(true);
}
// Check direct split-ring geometry and all RAM extents.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
void VirtioNet::validate_queue(const Queue &q) const {
    if (!q.size || q.size > MaximumQueueEntries || (q.size & (q.size - 1)) ||
        q.descriptors % virtio::DescriptorBytes || q.available % virtio::AvailableEntryBytes ||
        q.used % sizeof(uint32_t))
        throw std::runtime_error("Invalid virtio-net split queue geometry");
    memory(q.descriptors, q.size * virtio::DescriptorBytes);
    memory(q.available, virtio::RingFixedBytesWithEvent + q.size * virtio::AvailableEntryBytes);
    memory(q.used, virtio::RingFixedBytesWithEvent + q.size * virtio::UsedEntryBytes);
}
// Apply feature/status/queue writes, acknowledge interrupt causes or process transmit notification.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
void VirtioNet::write(unsigned offset, uint64_t value64, unsigned width) {
    if (width != 4 || offset % 4)
        throw std::runtime_error("Invalid virtio-net register write");
    uint32_t value = value64;
    if (offset == virtio::Register::StatusRegister && !value) {
        ++resetCount;
        queues = {};
        featureSelect = driverSelect = queueSelect = status = isr = 0;
        driverFeatures = 0;
        failure.clear();
        outgoing.clear();
        interrupt(false);
        return;
    }
    if (status & FailureStatus)
        return;
    try {
        if (offset == virtio::Register::DeviceFeaturesSelectorRegister) {
            featureSelect = value;
            return;
        }
        if (offset == virtio::Register::DriverFeaturesSelectorRegister) {
            driverSelect = value;
            return;
        }
        if (offset == virtio::Register::DriverFeaturesRegister) {
            if (status & virtio::FeaturesOk)
                throw std::runtime_error("Changing negotiated virtio-net features");
            if (driverSelect < FeatureWordCount) {
                unsigned shift = driverSelect * FeatureWordBits;
                driverFeatures =
                    (driverFeatures & ~(LowAddressMask << shift)) | (uint64_t(value) << shift);
            }
            return;
        }
        if (offset == virtio::Register::QueueSelectorRegister) {
            queueSelect = value;
            return;
        }
        if (offset == virtio::Register::InterruptAcknowledgeRegister) {
            isr &= ~value;
            interrupt(isr != 0);
            return;
        }
        if (offset == virtio::Register::StatusRegister) {
            status = value & UINT8_MAX;
            if ((status & virtio::FeaturesOk) &&
                ((driverFeatures & ~features) || !(driverFeatures & virtio::Version1Feature)))
                status &= ~uint32_t(virtio::FeaturesOk);
            if ((status & virtio::DriverOk) && !(status & virtio::FeaturesOk))
                throw std::runtime_error("virtio-net DRIVER_OK without FEATURES_OK");
            return;
        }
        if (offset == virtio::Register::QueueNotifyRegister) {
            if (value == QueueIndex::TransmitQueue)
                process_transmit_queue();
            return;
        }
        if (queueSelect >= QueueCount)
            return;
        auto &q = queues[queueSelect];
        if (offset == virtio::Register::QueueReadyRegister) {
            if (value > 1)
                throw std::runtime_error("Invalid virtio-net queue enable");
            if (value) {
                validate_queue(q);
                q.ready = 1;
            } else {
                q.ready = q.last = q.index = 0;
            }
            return;
        }
        if (offset == virtio::Register::QueueSizeRegister ||
            (offset >= virtio::Register::QueueDescriptorLowRegister &&
             offset <= virtio::Register::QueueDeviceHighRegister)) {
            if (q.ready)
                throw std::runtime_error("Changing active virtio-net queue");
            if (offset == virtio::Register::QueueSizeRegister)
                q.size = value;
            else if ((offset & QueueAddressRegisterStrideMask) == 0 ||
                     (offset & QueueAddressRegisterStrideMask) == HighAddressWordOffset) {
                auto &address = offset < virtio::Register::QueueDriverLowRegister   ? q.descriptors
                                : offset < virtio::Register::QueueDeviceLowRegister ? q.available
                                                                                    : q.used;
                address = (offset & HighAddressWordOffset)
                              ? (address & LowAddressMask) | (uint64_t(value) << FeatureWordBits)
                              : (address & HighAddressMask) | value;
            }
        }
    } catch (const std::runtime_error &e) {
        require_device_reset(e.what());
    }
}
// Validate a bounded direct chain whose buffers all have the required direction.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-430005
std::vector<VirtioNet::Buffer> VirtioNet::descriptor_chain(const Queue &q, uint16_t head,
                                                           bool writable) const {
    std::bitset<MaximumQueueEntries> seen;
    std::vector<Buffer> buffers;
    size_t total = 0;
    for (;;) {
        if (head >= q.size || seen[head])
            throw std::runtime_error("Invalid/cyclic virtio-net descriptor");
        seen.set(head);
        auto d = q.descriptors + head * virtio::DescriptorBytes;
        auto address = read_guest_value(d, 8);
        auto length = uint32_t(read_guest_value(d + virtio::DescriptorField::Length, 4));
        auto flags = read_guest_value(d + virtio::DescriptorField::Flags, 2);
        if ((flags & ~uint32_t(virtio::DescriptorNext | virtio::DescriptorWrite)) ||
            bool(flags & virtio::DescriptorWrite) != writable)
            throw std::runtime_error("Invalid virtio-net descriptor direction/flags");
        memory(address, length);
        total += length;
        if (total > MaximumChainBytes)
            throw std::runtime_error("Oversized virtio-net buffer chain");
        buffers.push_back({address, length});
        if (!(flags & virtio::DescriptorNext))
            return buffers;
        head = read_guest_value(d + virtio::DescriptorField::Next, 2);
    }
}
// Gather validated readable buffers into one packet including its Virtio header.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
VirtioNet::Bytes VirtioNet::gather_readable_buffers(const std::vector<Buffer> &buffers) const {
    Bytes data;
    for (auto b : buffers) {
        auto *p = memory(b.address, b.length);
        data.insert(data.end(), p, p + b.length);
    }
    return data;
}
// Copy a complete receive packet across validated writable buffers or reject short capacity.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
void VirtioNet::scatter_received_frame(const std::vector<Buffer> &buffers, const Bytes &packet) {
    size_t capacity = 0;
    for (auto b : buffers)
        capacity += b.length;
    if (capacity < packet.size())
        throw std::runtime_error("Short virtio-net receive buffer");
    size_t offset = 0;
    for (auto b : buffers) {
        auto size = std::min<size_t>(b.length, packet.size() - offset);
        std::memcpy(memory(b.address, size), packet.data() + offset, size);
        offset += size;
        if (offset == packet.size())
            break;
    }
}
// Return outstanding available entries using wrapping indices; reject ring overrun.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-490006
uint16_t VirtioNet::available_count(const Queue &q) const {
    auto count = uint16_t(read_guest_value(q.available + virtio::RingIndexOffset, 2) - q.last);
    if (count > q.size)
        throw std::runtime_error("virtio-net available ring overrun");
    return count;
}
// Publish used id/length/index and, with EVENT_IDX, request the next available index.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-540008
void VirtioNet::publish_used_buffer(Queue &q, uint16_t head, uint32_t length) {
    auto entry = q.used + virtio::RingEntriesOffset + (q.index % q.size) * virtio::UsedEntryBytes;
    write_guest_value(entry, head, 4);
    write_guest_value(entry + virtio::UsedLengthOffset, length, 4);
    ++q.index;
    ++q.last;
    write_guest_value(q.used + virtio::RingIndexOffset, q.index, 2);
    if (driverFeatures & (1ull << EventIndexFeatureBit))
        write_guest_value(q.used + virtio::RingEntriesOffset + q.size * virtio::UsedEntryBytes,
                          q.last, 2);
}
// Raise used-buffer interrupt only when published entries cross the driver notification threshold.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-510007
void VirtioNet::signal_used_buffers(Queue &q, uint16_t previous) {
    bool send;
    if (driverFeatures & (1ull << EventIndexFeatureBit)) {
        auto event = uint16_t(read_guest_value(
            q.available + virtio::RingEntriesOffset + q.size * virtio::AvailableEntryBytes, 2));
        // Wrapping 16-bit comparison asks whether publication crossed used_event.
        // Virtio 1.2, 2.7.7 Used Buffer Notification Suppression.
        // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-510007
        send = uint16_t(q.index - event - 1) < uint16_t(q.index - previous);
    } else
        send = !(read_guest_value(q.available + virtio::RingFlagsOffset, 2) &
                 virtio::AvailableNoInterrupt);
    if (send && previous != q.index) {
        signaledQueue = unsigned(&q - queues.data());
        isr |= virtio::UsedBufferInterrupt;
        interrupt(true);
    }
}
// Validate transmit frames, complete optional checksum and queue/drop host frames; publish
// completions. Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
void VirtioNet::process_transmit_queue() {
    auto &q = queues[QueueIndex::TransmitQueue];
    if (!q.ready || (status & InitializedStatus) != InitializedStatus || (status & FailureStatus))
        return;
    validate_queue(q);
    auto previous = q.index;
    auto count = available_count(q);
    while (count--) {
        auto head = uint16_t(read_guest_value(q.available + virtio::RingEntriesOffset +
                                                  (q.last % q.size) * virtio::AvailableEntryBytes,
                                              2));
        auto packet = gather_readable_buffers(descriptor_chain(q, head, false));
        if (packet.size() < PacketHeaderBytes + MinimumFrameBytes ||
            packet.size() > PacketHeaderBytes + MaximumFrameBytes ||
            packet[PacketHeaderField::SegmentationType])
            throw std::runtime_error("Unsupported virtio-net packet size/segmentation");
        if (packet[PacketHeaderField::Flags] & NeedsChecksum) {
            if (!(driverFeatures & (1ull << ChecksumFeatureBit)))
                throw std::runtime_error("Unnegotiated virtio-net checksum offload");
            unsigned start = packet[PacketHeaderField::ChecksumStart] |
                             (packet[PacketHeaderField::ChecksumStart + 1] << 8),
                     offset = packet[PacketHeaderField::ChecksumOffset] |
                              (packet[PacketHeaderField::ChecksumOffset + 1] << 8);
            if (start > packet.size() - PacketHeaderBytes ||
                offset + ChecksumWordBytes > packet.size() - PacketHeaderBytes - start)
                throw std::runtime_error("Invalid virtio-net checksum range");
            // Sum big-endian 16-bit words including the driver's pseudo-header seed,
            // pad an odd final byte with zero, fold end-around carries and complement.
            // Virtio 1.2 checksum offload; RFC 1071, section 1.
            // https://www.rfc-editor.org/rfc/rfc1071.html#section-1
            uint32_t sum = 0;
            for (size_t i = PacketHeaderBytes + start; i < packet.size(); i += ChecksumWordBytes)
                sum += (unsigned(packet[i]) << 8) | (i + 1 < packet.size() ? packet[i + 1] : 0);
            while (sum >> ChecksumWordBits)
                sum = (sum & ChecksumWordMask) + (sum >> ChecksumWordBits);
            auto at = PacketHeaderBytes + start + offset;
            packet[at] = (~sum) >> 8;
            packet[at + 1] = ~sum;
        }
        if (outgoing.size() < MaximumOutgoingFrames) {
            outgoing.emplace_back(packet.begin() + PacketHeaderBytes, packet.end());
            ++transmitted;
        } else
            ++dropped;
        publish_used_buffer(q, head, 0);
    }
    signal_used_buffers(q, previous);
}
// Consume one posted receive chain for an incoming frame or count a drop; notify completion.
// Virtio 1.2; backend packet/resource limits are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2170001
void VirtioNet::receive(const uint8_t *data, size_t length) {
    auto &q = queues[QueueIndex::ReceiveQueue];
    if (!q.ready || (status & InitializedStatus) != InitializedStatus || (status & FailureStatus) ||
        length < MinimumFrameBytes || length > MaximumFrameBytes) {
        ++dropped;
        return;
    }
    try {
        validate_queue(q);
        if (!available_count(q)) {
            ++dropped;
            return;
        }
        auto head = uint16_t(read_guest_value(q.available + virtio::RingEntriesOffset +
                                                  (q.last % q.size) * virtio::AvailableEntryBytes,
                                              2));
        auto buffers = descriptor_chain(q, head, true);
        Bytes packet(PacketHeaderBytes, 0);
        packet.insert(packet.end(), data, data + length);
        scatter_received_frame(buffers, packet);
        auto previous = q.index;
        publish_used_buffer(q, head, packet.size());
        signal_used_buffers(q, previous);
        ++received;
    } catch (const std::runtime_error &e) {
        require_device_reset(e.what());
    }
}
} // namespace pc
