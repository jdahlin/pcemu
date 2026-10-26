// Virtio block presents a sector-addressed disk through a split request queue.
// The driver negotiates modern features, publishes the queue addresses and posts
// a readable request header followed by data and a writable completion byte.
// Read (IN) requests write disk bytes to guest buffers; Write (OUT) requests read
// guest bytes into the disk. FLUSH calls the backing store's flush operation;
// GET_ID returns a stable model identifier. The used-ring length counts bytes
// written to the guest, including the final status byte, not bytes read from it.
//
// IMPLEMENTATION NOTE: Guest CPUs are stopped during synchronous processing.
// Complete chains are validated before disk I/O, and transfers stream through a
// 128 KiB scratch buffer up to a 1 GiB aggregate host bound. This permits firmware
// image reads that do not negotiate SIZE_MAX without allocating the entire request.
// Disk backend errors become IOERR; malformed queues set DEVICE_NEEDS_RESET with
// a host diagnostic. Device ID, configuration and status follow Virtio 1.2, 5.2.
// There is one queue; no indirect/packed descriptors, EVENT_IDX or multi-queue.
// COMPATIBILITY: QueueReady=0 clears indices without negotiated RING_RESET, status
// writes replace flags, and queue-address readback ignores an absent QueueSel.
// FIXME: Enforce modern queue-reset/initialization rules before claiming full
// transport conformance; preserve these guest-visible behaviors in this migration.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2740002
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2850006
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002

#include "virtio_blk.hpp"
#include "virtio_registers.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
namespace pc {
namespace {
// Virtio 1.2, 5.2: block device type and sole request virtqueue.
constexpr uint32_t BlockDeviceId = 2;
constexpr unsigned RequestQueue = 0;
// IMPLEMENTATION NOTE: Bound descriptor tracking, not the protocol's maximum queue size.
constexpr unsigned MaximumQueueEntries = 256;
// Feature words and guest addresses use low/high 32-bit transport halves.
constexpr unsigned FeatureWordBits = 32, FeatureWordCount = 2, BitsPerByte = 8;
constexpr uint64_t LowAddressMask = 0xffffffffull, HighAddressMask = 0xffffffff00000000ull;
constexpr uint32_t InitializedStatus =
    virtio::Acknowledge | virtio::Driver | virtio::FeaturesOk | virtio::DriverOk;
constexpr uint32_t FailureStatus = virtio::DeviceNeedsReset | virtio::Failed;
// IMPLEMENTATION NOTE: Retained 64-byte configuration window accepts reads starting
// no later than its final DWORD (even for byte reads); later byte starts are rejected.
constexpr unsigned DeviceConfigurationBytes = 64, DeviceConfigurationEnd = 0x140,
                   LastConfigurationReadOffset = 0x13c;
// Virtio 1.2, 5.2.4: device configuration field byte offsets.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2790004
enum ConfigurationField : unsigned {
    Capacity = 0,            // u64 number of 512-byte sectors, independent of blk_size.
    MaximumSegmentBytes = 8, // size_max:u32, valid with SIZE_MAX negotiated.
    MaximumSegments = 12,    // seg_max:u32, valid with SEG_MAX negotiated.
    LogicalBlockBytes = 20,  // blk_size:u32, valid with BLK_SIZE negotiated.
};
// Advertised per-segment and data-segment limits. The header/status need two more
// descriptors, leaving 126 data segments in the retained 128-descriptor convention.
constexpr uint32_t AdvertisedMaximumSegmentBytes = 128 * 1024, AdvertisedMaximumSegments = 126;
// Sector addressing always uses 512-byte units, regardless of logical block size.
constexpr unsigned SectorBytes = 512;
// Virtio 1.2, 5.2.6: request is type:u32,reserved:u32,sector:u64, then data/status.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2850006
enum RequestField : unsigned { Type = 0, Reserved = 4, Sector = 8 };
constexpr unsigned RequestHeaderBytes = 16, CompletionStatusBytes = 1;
enum RequestType : uint32_t {
    Read = 0,  // VIRTIO_BLK_T_IN: disk data goes into device-writable buffers.
    Write = 1, // VIRTIO_BLK_T_OUT: device-readable buffers supply disk data.
    Flush = 4, // VIRTIO_BLK_T_FLUSH: flush backing writes when negotiated.
    GetId = 8, // VIRTIO_BLK_T_GET_ID: return the 20-byte device identification string.
};
enum CompletionStatus : uint8_t {
    Success = 0,     // VIRTIO_BLK_S_OK: request completed successfully.
    IoError = 1,     // VIRTIO_BLK_S_IOERR: bounds, length or backing I/O failed.
    Unsupported = 2, // VIRTIO_BLK_S_UNSUPP: command/feature is not supported.
};
constexpr unsigned DeviceIdentificationBytes = 20;
// IMPLEMENTATION NOTE: Limit optional host tracing to its first 4096 messages.
constexpr unsigned MaximumTraceMessages = 4096;
// IMPLEMENTATION NOTE: Emit bounded host diagnostics when PCEMU_TRACE_VIRTIO_BLK is set.
// Register names/layout are Virtio 1.2, 4.2.2; tracing is not guest device behavior.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
void trace_block(const void *device, const char *operation, unsigned offset, uint64_t value,
                 unsigned width) {
    static const bool enabled = std::getenv("PCEMU_TRACE_VIRTIO_BLK") != nullptr;
    static unsigned remaining = MaximumTraceMessages;
    if (enabled && remaining) {
        --remaining;
        std::fprintf(stderr, "virtio-blk[%p] %s offset=%#x value=%#llx width=%u\n", device,
                     operation, offset, static_cast<unsigned long long>(value), width);
    }
}
// EDK II does not negotiate SIZE_MAX and submits whole EFI images in one
// descriptor. Bound aggregate I/O, but stream it through a small host buffer.
// IMPLEMENTATION NOTE: Aggregate cap independent of advertised per-segment size.
constexpr uint64_t MaximumRequestBytes = 1024ull * 1024 * 1024;
// IMPLEMENTATION NOTE: Bound host scratch allocation while streaming large requests.
constexpr size_t TransferChunkBytes = 128 * 1024;
// Decode a validated little-endian ring/configuration/request field.
// Virtio 1.2, 2.7 and 5.2.6.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2850006
uint64_t read_little_endian(const uint8_t *p, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(p[i]) << (i * BitsPerByte);
    return value;
}
} // namespace
// Bind the disk, guest RAM and interrupt callback for the single request queue.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2850006
VirtioBlock::VirtioBlock(Disk &d, std::span<uint8_t> r, uint64_t base,
                         std::function<void(bool)> irq)
    : disk(d), ram(r), ramBase(base), interrupt(std::move(irq)) {}
// Resolve a guest physical range with overflow-safe bound checks.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
uint8_t *VirtioBlock::memory(uint64_t address, uint64_t length) const {
    if (address < ramBase || length > ram.size() || address - ramBase > ram.size() - length)
        throw std::runtime_error("virtio DMA outside guest RAM");
    return ram.data() + (address - ramBase);
}
// Read a little-endian ring field from validated guest RAM.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
uint64_t VirtioBlock::read_guest_value(uint64_t address, unsigned width) const {
    return read_little_endian(memory(address, width), width);
}
// Publish a little-endian ring field to validated guest RAM.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
void VirtioBlock::write_guest_value(uint64_t address, uint64_t value, unsigned width) {
    auto *p = memory(address, width);
    for (unsigned i = 0; i < width; ++i)
        p[i] = uint8_t(value >> (i * BitsPerByte));
}
// Clear transport/queue/error state and lower interrupt; retain host I/O counters.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
void VirtioBlock::reset() {
    featureSelect = driverSelect = queueSelect = queueSize = ready = status = interruptStatus = 0;
    driverFeatures = descriptors = available = used = 0;
    lastAvailable = usedIndex = 0;
    failure.clear();
    interrupt(false);
}
// Read MMIO transport state or a synthesized little-endian block configuration.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2790004
uint64_t VirtioBlock::read(unsigned offset, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio MMIO read width/alignment");
    if (offset >= virtio::Register::DeviceConfigurationRegisters &&
        offset <= LastConfigurationReadOffset && width <= DeviceConfigurationEnd - offset) {
        std::array<uint8_t, DeviceConfigurationBytes> config{};
        // Build virtio_blk_config without native padding; only offered fields are populated.
        // Virtio 1.2, 5.2.4 device configuration layout.
        // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2790004
        auto write_configuration_field = [&](ConfigurationField field, uint64_t value,
                                             unsigned bytes) {
            for (unsigned i = 0; i < bytes; ++i)
                config[unsigned(field) + i] = uint8_t(value >> (BitsPerByte * i));
        };
        write_configuration_field(ConfigurationField::Capacity, disk.sectors, sizeof(uint64_t));
        write_configuration_field(ConfigurationField::MaximumSegmentBytes,
                                  AdvertisedMaximumSegmentBytes, sizeof(uint32_t));
        write_configuration_field(ConfigurationField::MaximumSegments, AdvertisedMaximumSegments,
                                  sizeof(uint32_t));
        write_configuration_field(ConfigurationField::LogicalBlockBytes, SectorBytes,
                                  sizeof(uint32_t));
        return read_little_endian(
            config.data() + offset - virtio::Register::DeviceConfigurationRegisters, width);
    }
    if (width != 4)
        throw std::runtime_error("virtio control registers require 32-bit access");
    switch (offset) {
    case virtio::Register::MagicValueRegister:
        return virtio::MmioMagicValue;
    case virtio::Register::VersionRegister:
        return virtio::ModernMmioVersion;
    case virtio::Register::DeviceIdRegister:
        return BlockDeviceId; // block device
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
        return queueSelect == RequestQueue ? MaximumQueueEntries : 0;
    case virtio::Register::QueueSizeRegister:
        return queueSelect == RequestQueue ? queueSize : 0;
    case virtio::Register::QueueReadyRegister:
        return queueSelect == RequestQueue ? ready : 0;
    case virtio::Register::InterruptStatusRegister:
        return interruptStatus;
    case virtio::Register::StatusRegister:
        return status;
    case virtio::Register::QueueDescriptorLowRegister:
        return uint32_t(descriptors);
    case virtio::Register::QueueDescriptorHighRegister:
        return descriptors >> FeatureWordBits;
    case virtio::Register::QueueDriverLowRegister:
        return uint32_t(available);
    case virtio::Register::QueueDriverHighRegister:
        return available >> FeatureWordBits;
    case virtio::Register::QueueDeviceLowRegister:
        return uint32_t(used);
    case virtio::Register::QueueDeviceHighRegister:
        return used >> FeatureWordBits;
    case virtio::Register::ConfigurationGenerationRegister:
        return 0;
    default:
        return 0; // reserved/read-only config fields
    }
}
// Record a queue failure, set DEVICE_NEEDS_RESET and assert configuration interrupt.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
void VirtioBlock::require_device_reset(const std::string &reason) {
    trace_block(this, reason.c_str(), virtio::Register::StatusRegister, status, 4);
    failure = reason;
    status |= virtio::DeviceNeedsReset; // DEVICE_NEEDS_RESET
    interruptStatus |= virtio::ConfigurationChangeInterrupt;
    interrupt(true);
}
// Validate split-ring geometry and all RAM extents before queue traversal.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
void VirtioBlock::validate_queue() const {
    if (!queueSize || queueSize > MaximumQueueEntries || (queueSize & (queueSize - 1)) ||
        descriptors % virtio::DescriptorBytes || available % virtio::AvailableEntryBytes ||
        used % sizeof(uint32_t))
        throw std::runtime_error("Invalid virtio split queue geometry");
    memory(descriptors, uint64_t(queueSize) * virtio::DescriptorBytes);
    memory(available,
           virtio::RingFixedBytesWithEvent + uint64_t(queueSize) * virtio::AvailableEntryBytes);
    memory(used, virtio::RingFixedBytesWithEvent + uint64_t(queueSize) * virtio::UsedEntryBytes);
}
// Apply modern MMIO feature, queue, status and interrupt-acknowledgement writes.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
void VirtioBlock::write(unsigned offset, uint64_t value64, unsigned width) {
    if (width != 4 || offset % 4)
        throw std::runtime_error("virtio registers require aligned 32-bit writes");
    trace_block(this, "write", offset, value64, width);
    uint32_t value = uint32_t(value64);
    if (offset == virtio::Register::StatusRegister && value == 0) {
        reset();
        return;
    }
    if (status & FailureStatus)
        return;
    // Replace one queue-address half before QueueReady; preserve the other half.
    // Virtio 1.2, 4.2.2 QueueDesc/QueueDriver/QueueDevice.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
    auto write_queue_address = [&](uint64_t &target, bool high) {
        if (ready) {
            require_device_reset("Changing an active virtqueue");
            return;
        }
        target = high ? (target & LowAddressMask) | (uint64_t(value) << FeatureWordBits)
                      : (target & HighAddressMask) | value;
    };
    switch (offset) {
    case virtio::Register::DeviceFeaturesSelectorRegister:
        featureSelect = value;
        break;
    case virtio::Register::DriverFeaturesSelectorRegister:
        driverSelect = value;
        break;
    case virtio::Register::DriverFeaturesRegister:
        if (status & virtio::FeaturesOk) {
            require_device_reset("Changing negotiated virtio features");
            break;
        }
        if (driverSelect < FeatureWordCount) {
            const auto shift = FeatureWordBits * driverSelect;
            driverFeatures =
                (driverFeatures & ~(LowAddressMask << shift)) | (uint64_t(value) << shift);
        }
        break;
    case virtio::Register::QueueSelectorRegister:
        queueSelect = value;
        break;
    case virtio::Register::QueueSizeRegister:
        if (queueSelect)
            break;
        if (ready) {
            require_device_reset("Changing active virtqueue size");
            break;
        }
        queueSize = value;
        break;
    case virtio::Register::QueueReadyRegister:
        if (queueSelect)
            break;
        if (value > 1) {
            require_device_reset("Invalid QueueReady");
            break;
        }
        if (value) {
            try {
                validate_queue();
                ready = 1;
            } catch (const std::runtime_error &e) {
                require_device_reset(e.what());
            }
        } else {
            ready = 0;
            lastAvailable = usedIndex = 0;
        }
        break;
    case virtio::Register::QueueNotifyRegister:
        if (value == 0)
            process_request_queue();
        break;
    case virtio::Register::InterruptAcknowledgeRegister:
        interruptStatus &= ~value;
        interrupt(interruptStatus != 0);
        break;
    case virtio::Register::StatusRegister:
        status = value & UINT8_MAX;
        if ((status & virtio::FeaturesOk) &&
            ((driverFeatures & ~features) || !(driverFeatures & virtio::Version1Feature)))
            status &= ~uint32_t(virtio::FeaturesOk);
        if ((status & virtio::DriverOk) && !(status & virtio::FeaturesOk))
            require_device_reset("DRIVER_OK without negotiated modern features");
        break;
    case virtio::Register::QueueDescriptorLowRegister:
        if (!queueSelect)
            write_queue_address(descriptors, false);
        break;
    case virtio::Register::QueueDescriptorHighRegister:
        if (!queueSelect)
            write_queue_address(descriptors, true);
        break;
    case virtio::Register::QueueDriverLowRegister:
        if (!queueSelect)
            write_queue_address(available, false);
        break;
    case virtio::Register::QueueDriverHighRegister:
        if (!queueSelect)
            write_queue_address(available, true);
        break;
    case virtio::Register::QueueDeviceLowRegister:
        if (!queueSelect)
            write_queue_address(used, false);
        break;
    case virtio::Register::QueueDeviceHighRegister:
        if (!queueSelect)
            write_queue_address(used, true);
        break;
    default:
        break; // writes to read-only/reserved registers
    }
}
// Collect validated direct buffers; reject cycles and readable-after-writable order.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-430005
std::vector<VirtioBlock::Buffer> VirtioBlock::descriptor_chain(uint16_t index) const {
    std::bitset<MaximumQueueEntries> visited;
    std::vector<Buffer> buffers;
    bool writing = false;
    for (;;) {
        if (index >= queueSize || visited[index])
            throw std::runtime_error("Invalid/cyclic virtio descriptor chain");
        visited.set(index);
        const auto desc = descriptors + uint64_t(index) * virtio::DescriptorBytes;
        uint64_t address = read_guest_value(desc, 8);
        uint32_t length = uint32_t(read_guest_value(desc + virtio::DescriptorField::Length, 4));
        uint16_t flags = uint16_t(read_guest_value(desc + virtio::DescriptorField::Flags, 2));
        if (flags & ~uint32_t(virtio::DescriptorNext | virtio::DescriptorWrite))
            throw std::runtime_error("Unsupported virtio descriptor flags");
        memory(address, length);
        bool writable = flags & virtio::DescriptorWrite;
        if (writing && !writable)
            throw std::runtime_error("Virtio readable buffer follows writable buffer");
        writing |= writable;
        buffers.push_back({address, length, writable});
        if (!(flags & virtio::DescriptorNext))
            return buffers;
        index = uint16_t(read_guest_value(desc + virtio::DescriptorField::Next, 2));
    }
}
// Copy a slice across readable or writable chain buffers, rejecting a short aggregate span.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2850006
void VirtioBlock::copy_chain_bytes(const std::vector<Buffer> &buffers, bool writable, size_t skip,
                                   std::span<uint8_t> bytes, bool toGuest) {
    for (const auto &b : buffers) {
        if (b.writable != writable)
            continue;
        if (skip >= b.length) {
            skip -= b.length;
            continue;
        }
        size_t length = std::min<size_t>(b.length - skip, bytes.size());
        auto *pointer = memory(b.address + skip, length);
        if (toGuest)
            std::memcpy(pointer, bytes.data(), length);
        else
            std::memcpy(bytes.data(), pointer, length);
        bytes = bytes.subspan(length);
        skip = 0;
        if (bytes.empty())
            return;
    }
    if (!bytes.empty())
        throw std::runtime_error("Short virtio scatter/gather chain");
}
// Execute IN/OUT/FLUSH/GET_ID, write completion status and return guest-written bytes.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2850006
uint32_t VirtioBlock::execute_request(uint16_t head) {
    // Streaming is not atomic: a later backing error can follow earlier sector writes.
    // Counters and used payload length are updated only for whole-request success.
    const auto buffers =
        descriptor_chain(head); // Validate entire DMA chain before touching disk or guest buffers.
    size_t readable = 0, writable = 0;
    for (const auto &b : buffers)
        (b.writable ? writable : readable) += b.length;
    if (readable < RequestHeaderBytes || writable < CompletionStatusBytes)
        throw std::runtime_error("Short virtio block header/status");
    std::array<uint8_t, RequestHeaderBytes> header{};
    copy_chain_bytes(buffers, false, 0, header, false);
    uint32_t type = uint32_t(read_little_endian(header.data(), 4));
    uint64_t sector = read_little_endian(header.data() + RequestField::Sector, 8);
    uint8_t result = CompletionStatus::Unsupported;
    uint32_t usedLength = CompletionStatusBytes;
    if (type == RequestType::Read || type == RequestType::Write) {
        size_t length = type == RequestType::Read ? writable - CompletionStatusBytes
                                                  : readable - RequestHeaderBytes;
        if (length > MaximumRequestBytes || length % SectorBytes || sector > disk.sectors ||
            length / SectorBytes > disk.sectors - sector)
            result = CompletionStatus::IoError;
        else {
            std::vector<uint8_t> data(std::min(length, TransferChunkBytes));
            bool success = true;
            for (size_t offset = 0; offset < length && success; offset += TransferChunkBytes) {
                auto part =
                    std::span<uint8_t>(data).first(std::min(TransferChunkBytes, length - offset));
                if (type == RequestType::Read) {
                    success = disk.read(sector + offset / SectorBytes,
                                        unsigned(part.size() / SectorBytes), part.data());
                    if (success)
                        copy_chain_bytes(buffers, true, offset, part, true);
                } else {
                    copy_chain_bytes(buffers, false, RequestHeaderBytes + offset, part, false);
                    success = disk.write(sector + offset / SectorBytes,
                                         unsigned(part.size() / SectorBytes), part.data());
                }
            }
            if (success) {
                if (type == RequestType::Read) {
                    bytesRead += length;
                    usedLength += length;
                } else
                    bytesWritten += length;
            }
            result = success ? CompletionStatus::Success : CompletionStatus::IoError;
        }
    } else if (type == RequestType::Flush) {
        if (!(driverFeatures & (1ull << FlushFeatureBit)))
            result = CompletionStatus::Unsupported;
        else {
            disk.flush();
            result = CompletionStatus::Success;
        }
    } else if (type == RequestType::GetId) {
        if (writable - CompletionStatusBytes < DeviceIdentificationBytes)
            result = CompletionStatus::IoError;
        else {
            std::array<uint8_t, DeviceIdentificationBytes> id{'P', 'C', 'E', 'M', 'U', '-', 'V',
                                                              'I', 'R', 'T', 'I', 'O', '-', 'B',
                                                              'L', 'K', '0', '0', '0', '1'};
            copy_chain_bytes(buffers, true, 0, id, true);
            result = CompletionStatus::Success;
            usedLength += DeviceIdentificationBytes;
        }
    }
    copy_chain_bytes(buffers, true, writable - CompletionStatusBytes,
                     std::span<uint8_t>(&result, 1), true);
    ++requests;
    return usedLength;
}
// Consume available requests, publish used indices/lengths and unsuppressed interrupts.
// Virtio 1.2; RAM, streaming and trace bounds are host implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2850006
void VirtioBlock::process_request_queue() {
    if (!ready || (status & InitializedStatus) != InitializedStatus || (status & FailureStatus))
        return;
    try {
        validate_queue();
        uint16_t next = uint16_t(read_guest_value(available + virtio::RingIndexOffset, 2));
        if (uint16_t(next - lastAvailable) > queueSize)
            throw std::runtime_error("Virtio available ring overrun");
        bool completed = lastAvailable != next;
        while (lastAvailable != next) {
            auto head = uint16_t(read_guest_value(available + virtio::RingEntriesOffset +
                                                      uint64_t(lastAvailable % queueSize) *
                                                          virtio::AvailableEntryBytes,
                                                  2));
            auto length = execute_request(head);
            auto entry = used + virtio::RingEntriesOffset +
                         uint64_t(usedIndex % queueSize) * virtio::UsedEntryBytes;
            write_guest_value(entry, head, 4);
            write_guest_value(entry + virtio::UsedLengthOffset, length, 4);
            ++lastAvailable;
            ++usedIndex;
            write_guest_value(used + virtio::RingIndexOffset, usedIndex, 2);
        }
        if (completed && !(read_guest_value(available + virtio::RingFlagsOffset, 2) &
                           virtio::AvailableNoInterrupt)) {
            interruptStatus |= virtio::UsedBufferInterrupt;
            interrupt(true);
        }
    } catch (const std::runtime_error &e) {
        require_device_reset(e.what());
    }
}
} // namespace pc
