// Virtio devices exchange buffers through virtqueues in guest RAM. This shared
// core implements the modern MMIO register layout and split-ring format used by
// the console, balloon and GPU models. The driver negotiates features, supplies
// descriptor/available/used addresses, marks queues ready and sets DRIVER_OK.
// A notification consumes available chains: readable buffers carry requests,
// writable buffers receive responses, and used entries publish completion lengths.
// An interrupt callback carries used-buffer or configuration-change causes to the
// machine/PCI adapter; acknowledging causes lowers it once none remain pending.
//
// IMPLEMENTATION NOTE: All calls run with virtual CPUs stopped, so queue reads,
// backing device work and completion publication are serialized host operations.
// DMA is restricted to the bound RAM span, queues to 256 entries, chains to 16 MiB
// and configuration to the first 64 bytes. These are host bounds, not Virtio limits.
// Only direct split descriptors are accepted; packed/indirect rings, EVENT_IDX
// and per-queue reset are not implemented. The core always offers VERSION_1;
// derived devices must not advertise queue formats/features the core cannot serve.
// Invalid queue input records a host-readable reason and sets DEVICE_NEEDS_RESET;
// invalid register access throws. A handler may defer a chain without consuming it.
// FIXME: Status writes currently replace flags rather than enforcing every legal
// initialization transition. Keep this compatibility behavior during documentation.
// Virtio 1.2, 2.1/2.2, 2.7 Split Virtqueues, 3.1 initialization and 4.2 MMIO.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002

#include "virtio_device.hpp"
#include "virtio_registers.hpp"
#include <algorithm>
#include <bitset>
#include <stdexcept>

namespace pc {
namespace {
// IMPLEMENTATION NOTE: Limit registered queues (independent of per-queue entries).
constexpr unsigned MaximumQueueCount = 256;
// IMPLEMENTATION NOTE: Maximum total readable+writable bytes per request.
constexpr uint64_t MaximumChainBytes = 16 * 1024 * 1024;
// IMPLEMENTATION NOTE: Forward only the first 64 device-specific configuration bytes.
constexpr unsigned DeviceConfigurationEnd = 0x140;
// Feature words and guest physical addresses are transferred in 32-bit halves.
constexpr unsigned FeatureWordBits = 32, FeatureWordCount = 2, BitsPerByte = 8;
constexpr uint64_t LowAddressMask = 0xffffffffull, HighAddressMask = 0xffffffff00000000ull;
// Either device- or driver-reported failure stops further queue configuration.
constexpr uint32_t FailureStatus = virtio::DeviceNeedsReset | virtio::Failed;
} // namespace
// Bind RAM, type/features and interrupt callback; reject an invalid host queue count.
// Virtio 1.2; RAM bounds and serialized execution are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
VirtioDevice::VirtioDevice(std::span<uint8_t> r, uint64_t base, uint32_t deviceId, unsigned count,
                           uint64_t bits, Interrupt irq)
    : ram(r), ramBase(base), features(bits | virtio::Version1Feature), id(deviceId), queues(count),
      interrupt(std::move(irq)) {
    if (!count || count > MaximumQueueCount)
        throw std::runtime_error("Invalid virtio queue count");
}
// Resolve a guest physical span, rejecting overflow and accesses outside bound RAM.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
std::span<uint8_t> VirtioDevice::memory(uint64_t address, uint64_t length) const {
    if (address < ramBase || length > ram.size() || address - ramBase > ram.size() - length)
        throw std::runtime_error("Virtio DMA outside guest RAM");
    return ram.subspan(address - ramBase, length);
}
// Read a little-endian split-ring field from validated guest RAM.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
uint64_t VirtioDevice::read_guest_value(uint64_t address, unsigned width) const {
    auto bytes = memory(address, width);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(bytes[i]) << (i * BitsPerByte);
    return value;
}
// Publish a little-endian split-ring field to validated guest RAM.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
void VirtioDevice::write_guest_value(uint64_t address, uint64_t value, unsigned width) {
    auto bytes = memory(address, width);
    for (unsigned i = 0; i < width; ++i)
        bytes[i] = uint8_t(value >> (i * BitsPerByte));
}
// Clear transport/queue/error state, reset the derived device and lower its interrupt.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
void VirtioDevice::reset() {
    ++resetCount;
    featureSelect = driverSelect = queueSelect = status = interruptStatus = generation = 0;
    driverFeatures = 0;
    std::fill(queues.begin(), queues.end(), Queue{});
    failure.clear();
    reset_device();
    interrupt(false, 0);
}
// Record a processing failure, set DEVICE_NEEDS_RESET and notify configuration change.
// Virtio 1.2; RAM bounds and serialized execution are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
void VirtioDevice::require_device_reset(const std::string &reason) {
    failure = reason;
    status |= virtio::DeviceNeedsReset;
    configuration_changed();
}
// Advance ConfigGeneration and assert the configuration-change interrupt cause.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
void VirtioDevice::configuration_changed() {
    ++generation;
    interruptStatus |= virtio::ConfigurationChangeInterrupt;
    interrupt(true, lastInterruptQueue);
}
// Read MMIO transport state or a bounded device-specific configuration field.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
uint64_t VirtioDevice::read(unsigned offset, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio register access");
    if (offset >= virtio::Register::DeviceConfigurationRegisters &&
        offset < DeviceConfigurationEnd && width <= DeviceConfigurationEnd - offset)
        return read_device_configuration(offset - virtio::Register::DeviceConfigurationRegisters,
                                         width);
    if (width != 4)
        throw std::runtime_error("Virtio controls require 32-bit access");
    const auto *q = queueSelect < queues.size() ? &queues[queueSelect] : nullptr;
    switch (offset) {
    case virtio::Register::MagicValueRegister:
        return virtio::MmioMagicValue;
    case virtio::Register::VersionRegister:
        return virtio::ModernMmioVersion;
    case virtio::Register::DeviceIdRegister:
        return id;
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
        return q ? MaximumQueueEntries : 0;
    case virtio::Register::QueueSizeRegister:
        return q ? q->size : 0;
    case virtio::Register::QueueReadyRegister:
        return q && q->ready;
    case virtio::Register::InterruptStatusRegister:
        return interruptStatus;
    case virtio::Register::StatusRegister:
        return status;
    case virtio::Register::QueueDescriptorLowRegister:
        return q ? uint32_t(q->descriptors) : 0;
    case virtio::Register::QueueDescriptorHighRegister:
        return q ? q->descriptors >> 32 : 0;
    case virtio::Register::QueueDriverLowRegister:
        return q ? uint32_t(q->available) : 0;
    case virtio::Register::QueueDriverHighRegister:
        return q ? q->available >> 32 : 0;
    case virtio::Register::QueueDeviceLowRegister:
        return q ? uint32_t(q->used) : 0;
    case virtio::Register::QueueDeviceHighRegister:
        return q ? q->used >> 32 : 0;
    case virtio::Register::ConfigurationGenerationRegister:
        return generation;
    default:
        return 0;
    }
}
// Validate split-queue size, alignment and complete RAM extents before use.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
void VirtioDevice::validate_queue(const Queue &q) const {
    if (!q.size || q.size > MaximumQueueEntries || (q.size & (q.size - 1)) ||
        q.descriptors % virtio::DescriptorBytes || q.available % virtio::AvailableEntryBytes ||
        q.used % sizeof(uint32_t))
        throw std::runtime_error("Invalid virtio split queue geometry");
    memory(q.descriptors, uint64_t(q.size) * virtio::DescriptorBytes);
    memory(q.available,
           virtio::RingFixedBytesWithEvent + uint64_t(q.size) * virtio::AvailableEntryBytes);
    memory(q.used, virtio::RingFixedBytesWithEvent + uint64_t(q.size) * virtio::UsedEntryBytes);
}
// Apply MMIO initialization, queue setup/notification and interrupt acknowledgement.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
void VirtioDevice::write(unsigned offset, uint64_t raw, unsigned width) {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio register access");
    if (offset >= virtio::Register::DeviceConfigurationRegisters &&
        offset < DeviceConfigurationEnd && width <= DeviceConfigurationEnd - offset) {
        write_device_configuration(offset - virtio::Register::DeviceConfigurationRegisters, raw,
                                   width);
        return;
    }
    if (width != 4)
        throw std::runtime_error("Virtio controls require 32-bit access");
    uint32_t value = uint32_t(raw);
    if (offset == virtio::Register::StatusRegister && !value) {
        reset();
        return;
    }
    if (offset == virtio::Register::InterruptAcknowledgeRegister) {
        interruptStatus &= ~value;
        interrupt(interruptStatus != 0, lastInterruptQueue);
        return;
    }
    if (status & FailureStatus)
        return;
    auto *q = queueSelect < queues.size() ? &queues[queueSelect] : nullptr;
    // Replace one address half only before QueueReady; active queue mutation needs reset.
    // Virtio 1.2, 4.2.2 QueueDesc/QueueDriver/QueueDevice.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
    auto write_queue_address = [&](uint64_t &target, bool high) {
        if (q->ready) {
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
            auto shift = driverSelect * FeatureWordBits;
            driverFeatures =
                (driverFeatures & ~(LowAddressMask << shift)) | (uint64_t(value) << shift);
        }
        break;
    case virtio::Register::QueueSelectorRegister:
        queueSelect = value;
        break;
    case virtio::Register::QueueSizeRegister:
        if (!q)
            break;
        if (q->ready)
            require_device_reset("Changing active virtqueue size");
        else
            q->size = value;
        break;
    case virtio::Register::QueueReadyRegister:
        if (!q)
            break;
        if (value > 1) {
            require_device_reset("Invalid QueueReady");
            break;
        }
        if (value) {
            try {
                validate_queue(*q);
                q->ready = true;
            } catch (const std::runtime_error &e) {
                require_device_reset(e.what());
            }
        } else if (q->ready)
            require_device_reset("Virtqueue reset was not negotiated");
        break;
    case virtio::Register::QueueNotifyRegister:
        if (value < queues.size() && running())
            notified(value);
        break;
    case virtio::Register::StatusRegister:
        status = value & UINT8_MAX;
        if ((status & virtio::FeaturesOk) &&
            ((driverFeatures & ~features) || !(driverFeatures & virtio::Version1Feature)))
            status &= ~uint32_t(virtio::FeaturesOk);
        if ((status & virtio::DriverOk) && !(status & virtio::FeaturesOk))
            require_device_reset("DRIVER_OK without modern features");
        break;
    case virtio::Register::QueueDescriptorLowRegister:
        if (q)
            write_queue_address(q->descriptors, false);
        break;
    case virtio::Register::QueueDescriptorHighRegister:
        if (q)
            write_queue_address(q->descriptors, true);
        break;
    case virtio::Register::QueueDriverLowRegister:
        if (q)
            write_queue_address(q->available, false);
        break;
    case virtio::Register::QueueDriverHighRegister:
        if (q)
            write_queue_address(q->available, true);
        break;
    case virtio::Register::QueueDeviceLowRegister:
        if (q)
            write_queue_address(q->used, false);
        break;
    case virtio::Register::QueueDeviceHighRegister:
        if (q)
            write_queue_address(q->used, true);
        break;
    default:
        break;
    }
}
// Walk a bounded direct descriptor chain; reject cycles and readable-after-writable order.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-430005
VirtioDevice::Chain VirtioDevice::descriptor_chain(const Queue &q, uint16_t index) const {
    Chain result;
    std::bitset<MaximumQueueEntries> visited;
    bool writing = false;
    for (;;) {
        if (index >= q.size || visited[index])
            throw std::runtime_error("Invalid/cyclic virtio descriptor chain: index=" +
                                     std::to_string(index) + " size=" + std::to_string(q.size) +
                                     " descriptors=" + std::to_string(q.descriptors));
        visited.set(index);
        auto descriptor = q.descriptors + uint64_t(index) * virtio::DescriptorBytes;
        auto flags = read_guest_value(descriptor + virtio::DescriptorField::Flags, 2);
        if (flags & ~uint32_t(virtio::DescriptorNext | virtio::DescriptorWrite))
            throw std::runtime_error("Unsupported virtio descriptor flags");
        bool writable = (flags & virtio::DescriptorWrite) != 0;
        if (writing && !writable)
            throw std::runtime_error("Readable buffer follows writable virtio buffer");
        writing |= writable;
        auto bytes = memory(read_guest_value(descriptor, 8),
                            read_guest_value(descriptor + virtio::DescriptorField::Length, 4));
        result.segments.push_back({bytes, writable});
        (writable ? result.writable : result.readable) += bytes.size();
        if (result.readable + result.writable > MaximumChainBytes)
            throw std::runtime_error("Virtio descriptor chain exceeds processing bound");
        if (!(flags & virtio::DescriptorNext))
            break;
        index = uint16_t(read_guest_value(descriptor + virtio::DescriptorField::Next, 2));
    }
    return result;
}
// Gather device-readable segments, rejecting a request beyond the caller bound.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-430005
std::vector<uint8_t> VirtioDevice::Chain::input(size_t maximum) const {
    if (readable > maximum)
        throw std::runtime_error("Virtio request exceeds device bound");
    std::vector<uint8_t> result;
    result.reserve(readable);
    for (const auto &s : segments)
        if (!s.writable)
            result.insert(result.end(), s.bytes.begin(), s.bytes.end());
    return result;
}
// Scatter response bytes into writable chain segments; reject insufficient capacity.
// Virtio 1.2; RAM bounds and serialized execution are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-430005
void VirtioDevice::Chain::output(std::span<const uint8_t> bytes) const {
    if (bytes.size() > writable)
        throw std::runtime_error("Virtio response buffer too small");
    for (const auto &s : segments)
        if (s.writable) {
            auto count = std::min(s.bytes.size(), bytes.size());
            std::copy_n(bytes.begin(), count, s.bytes.begin());
            bytes = bytes.subspan(count);
            if (bytes.empty())
                break;
        }
}
// Consume available chains until exhausted/deferred, publish used entries and notify the driver.
// Virtio 1.2; host RAM bounds and serialized execution are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-540008
void VirtioDevice::process_queue(unsigned queue) {
    if (!running() || queue >= queues.size() || !queues[queue].ready)
        return;
    auto &q = queues[queue];
    try {
        validate_queue(q);
        auto next = uint16_t(read_guest_value(q.available + virtio::RingIndexOffset, 2));
        if (uint16_t(next - q.nextAvailable) > q.size)
            throw std::runtime_error("Virtio available ring overrun");
        bool completed = false;
        while (q.nextAvailable != next) {
            auto head = uint16_t(read_guest_value(q.available + virtio::RingEntriesOffset +
                                                      uint64_t(q.nextAvailable % q.size) *
                                                          virtio::AvailableEntryBytes,
                                                  2));
            auto buffers = descriptor_chain(q, head);
            auto length = handle(queue, buffers);
            if (!length)
                break;
            if (*length > buffers.writable)
                throw std::runtime_error("Invalid virtio used length");
            auto entry = q.used + virtio::RingEntriesOffset +
                         uint64_t(q.nextUsed % q.size) * virtio::UsedEntryBytes;
            write_guest_value(entry, head, 4);
            write_guest_value(entry + virtio::UsedLengthOffset, *length, 4);
            ++q.nextAvailable;
            ++q.nextUsed;
            ++completions;
            write_guest_value(q.used + virtio::RingIndexOffset, q.nextUsed, 2);
            completed = true;
        }
        if (completed && !(read_guest_value(q.available + virtio::RingFlagsOffset, 2) &
                           virtio::AvailableNoInterrupt)) {
            lastInterruptQueue = queue;
            interruptStatus |= virtio::UsedBufferInterrupt;
            interrupt(true, queue);
        }
    } catch (const std::runtime_error &e) {
        require_device_reset("queue=" + std::to_string(queue) +
                             " avail=" + std::to_string(q.nextAvailable) +
                             " used=" + std::to_string(q.nextUsed) + " " + e.what());
    }
}
} // namespace pc
