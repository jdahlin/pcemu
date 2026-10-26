// Virtio entropy provides random bytes without emulating a physical random-number
// chip. The driver negotiates the modern transport, configures requestq and posts
// device-writable buffers. A notification validates the full direct descriptor
// chain, fills up to 64 KiB from the host entropy source, then publishes the actual
// used length. Partial fills are permitted; the guest must inspect that length.
// Used-ring completion interrupts follow the available ring's suppression flag.
//
// IMPLEMENTATION NOTE: Host arc4random_buf supplies the default random bytes;
// tests may inject a deterministic source. All processing occurs with CPUs stopped.
// Guest DMA is restricted to bound RAM, queues to 256 entries and random work to
// 64 KiB per chain. The transport has no device-specific configuration/features;
// VERSION_1 is the only offered transport feature. Indirect/packed queues and
// EVENT_IDX are unsupported. Malformed queues require device reset; malformed
// register accesses throw to the host caller. No entropy credit/rate policy exists.
// COMPATIBILITY: QueueReady=0 resets ring indices without negotiated RING_RESET,
// and status writes replace flags. FIXME: Enforce the required initialization and
// queue-reset rules before claiming full modern transport conformance; preserve
// current behavior during documentation. Register address readback is retained
// even when QueueSel names an absent queue; size/ready reads do report absence.
// Virtio 1.2, 5.4 Entropy Device, 2.7 split rings and 4.2.2 MMIO registers.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3050004
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3110006
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002

#include "virtio_rng.hpp"
#include "virtio_registers.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdlib>
#include <stdexcept>
namespace pc {
namespace {
// Virtio 1.2, 5.4.1/5.4.2: entropy device type and sole queue index.
constexpr uint32_t EntropyDeviceId = 4;
constexpr unsigned RequestQueue = 0;
// IMPLEMENTATION NOTE: Bound descriptor tracking and per-chain entropy work.
constexpr unsigned MaximumQueueEntries = 256, MaximumRequestBytes = 65536;
// IMPLEMENTATION NOTE: The retained configuration read window is 64 bytes of zero.
constexpr unsigned DeviceConfigurationEnd = 0x140;
// Feature selectors choose the low or high 32-bit word of the 64-bit feature set.
constexpr unsigned FeatureWordBits = 32, FeatureWordCount = 2, BitsPerByte = 8;
// Queue addresses are assembled from low/high 32-bit MMIO writes.
constexpr uint64_t LowAddressMask = 0xffffffffull, HighAddressMask = 0xffffffff00000000ull;
// All initialization flags are needed, and either failure flag stops queue work.
constexpr uint32_t InitializedStatus =
    virtio::Acknowledge | virtio::Driver | virtio::FeaturesOk | virtio::DriverOk;
constexpr uint32_t FailureStatus = virtio::DeviceNeedsReset | virtio::Failed;
// Decode a little-endian ring field from an already validated RAM pointer.
// Virtio 1.2, 2.7 Split Virtqueues.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
uint64_t read_little_endian(const uint8_t *p, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(p[i]) << (i * BitsPerByte);
    return value;
}
} // namespace
// Bind guest RAM/interrupt delivery and select the injected or host entropy source.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3110006
VirtioRng::VirtioRng(std::span<uint8_t> r, uint64_t base, std::function<void(bool)> irq,
                     Entropy source)
    : ram(r), ramBase(base), interrupt(std::move(irq)), entropy(std::move(source)) {
    // IMPLEMENTATION NOTE: Use the platform cryptographic random source unless
    // the host injected a source; Virtio specifies buffer contents, not this API.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3110006
    if (!entropy)
        entropy = [](std::span<uint8_t> bytes) { arc4random_buf(bytes.data(), bytes.size()); };
}
// Validate a guest physical range and return its host pointer, rejecting overflow.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
uint8_t *VirtioRng::memory(uint64_t address, uint64_t length) const {
    if (address < ramBase || length > ram.size() || address - ramBase > ram.size() - length)
        throw std::runtime_error("virtio DMA outside guest RAM");
    return ram.data() + (address - ramBase);
}
// Read a little-endian ring field from validated guest RAM.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
uint64_t VirtioRng::read_guest_value(uint64_t address, unsigned width) const {
    return read_little_endian(memory(address, width), width);
}
// Publish a little-endian ring field to validated guest RAM.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
void VirtioRng::write_guest_value(uint64_t address, uint64_t value, unsigned width) {
    auto *p = memory(address, width);
    for (unsigned i = 0; i < width; ++i)
        p[i] = uint8_t(value >> (i * BitsPerByte));
}
// Clear transport, queue and error state and lower interrupt; retain host byte counter.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
void VirtioRng::reset() {
    featureSelect = driverSelect = queueSelect = queueSize = ready = status = interruptStatus = 0;
    driverFeatures = descriptors = available = used = 0;
    lastAvailable = usedIndex = 0;
    failure.clear();
    interrupt(false);
}
// Read modern MMIO state; absent entropy configuration bytes read zero.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
uint64_t VirtioRng::read(unsigned offset, unsigned width) const {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width)
        throw std::runtime_error("Invalid virtio MMIO read width/alignment");
    if (offset >= virtio::Register::DeviceConfigurationRegisters &&
        offset < DeviceConfigurationEnd && width <= DeviceConfigurationEnd - offset)
        return 0;
    if (width != 4)
        throw std::runtime_error("virtio control registers require 32-bit access");
    switch (offset) {
    case virtio::Register::MagicValueRegister:
        return virtio::MmioMagicValue;
    case virtio::Register::VersionRegister:
        return virtio::ModernMmioVersion;
    case virtio::Register::DeviceIdRegister:
        return EntropyDeviceId; // entropy device
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
// Record a host-visible failure, set DEVICE_NEEDS_RESET and signal configuration change.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
void VirtioRng::require_device_reset(const std::string &reason) {
    failure = reason;
    status |= virtio::DeviceNeedsReset; // DEVICE_NEEDS_RESET
    interruptStatus |= virtio::ConfigurationChangeInterrupt;
    interrupt(true);
}
// Check direct split-ring geometry and RAM extents before touching descriptors.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-350007
void VirtioRng::validate_queue() const {
    if (!queueSize || queueSize > MaximumQueueEntries || (queueSize & (queueSize - 1)) ||
        descriptors % virtio::DescriptorBytes || available % virtio::AvailableEntryBytes ||
        used % sizeof(uint32_t))
        throw std::runtime_error("Invalid virtio split queue geometry");
    memory(descriptors, uint64_t(queueSize) * virtio::DescriptorBytes);
    memory(available,
           virtio::RingFixedBytesWithEvent + uint64_t(queueSize) * virtio::AvailableEntryBytes);
    memory(used, virtio::RingFixedBytesWithEvent + uint64_t(queueSize) * virtio::UsedEntryBytes);
}
// Apply MMIO feature/queue/status writes and interrupt acknowledgements.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
void VirtioRng::write(unsigned offset, uint64_t value64, unsigned width) {
    if (width != 4 || offset % 4)
        throw std::runtime_error("virtio registers require aligned 32-bit writes");
    uint32_t value = uint32_t(value64);
    if (offset == virtio::Register::StatusRegister && value == 0) {
        reset();
        return;
    }
    if (status & FailureStatus)
        return;
    // Replace only the selected address half; active queue mutation needs reset.
    // Virtio 1.2, 4.2.2 queue address registers.
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
// Validate a writable direct chain, fill up to the host limit and return used bytes.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3110006
uint32_t VirtioRng::fill_entropy_request(uint16_t index) {
    std::bitset<MaximumQueueEntries> visited;
    std::vector<std::span<uint8_t>> buffers;
    uint64_t capacity = 0;
    for (;;) {
        if (index >= queueSize || visited[index])
            throw std::runtime_error("Invalid/cyclic entropy descriptor chain");
        visited.set(index);
        auto descriptor = descriptors + uint64_t(index) * virtio::DescriptorBytes;
        auto address = read_guest_value(descriptor, 8);
        uint32_t length =
            uint32_t(read_guest_value(descriptor + virtio::DescriptorField::Length, 4));
        auto flags = read_guest_value(descriptor + virtio::DescriptorField::Flags, 2);
        if (!(flags & virtio::DescriptorWrite) ||
            (flags & ~uint32_t(virtio::DescriptorNext | virtio::DescriptorWrite)))
            throw std::runtime_error("Entropy descriptors must be writable, direct buffers");
        buffers.emplace_back(memory(address, length), length);
        capacity += length;
        if (!(flags & virtio::DescriptorNext))
            break;
        index = uint16_t(read_guest_value(descriptor + virtio::DescriptorField::Next, 2));
    }
    if (!capacity)
        throw std::runtime_error("Empty entropy request");
    // Validate the entire chain before any write. Partial fills are permitted;
    // cap CPU/entropy work even when a guest supplies very large buffers.
    uint32_t supplied = uint32_t(std::min<uint64_t>(capacity, MaximumRequestBytes));
    size_t remaining = supplied;
    for (auto buffer : buffers) {
        auto length = std::min(buffer.size(), remaining);
        if (length)
            entropy(buffer.first(length));
        remaining -= length;
        if (!remaining)
            break;
    }
    bytesSupplied += supplied;
    return supplied;
}
// Fill available request chains, publish lengths/indices and signal unsuppressed completions.
// Virtio 1.2; host RAM/resource limits are implementation policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3110006
void VirtioRng::process_request_queue() {
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
            auto length = fill_entropy_request(head);
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
