#pragma once
#include <cstdint>

namespace pc::virtio {
// Virtio 1.2, 4.2.2 MMIO Device Register Layout. These offsets belong to
// the MMIO transport, even when a PCI adapter translates accesses into it.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
enum Register : unsigned {
    // Read-only ASCII “virt” discovery signature.
    MagicValueRegister = 0x0,
    // Transport version: 2 selects the modern MMIO interface.
    VersionRegister = 0x4,
    // Virtio device type; zero means no device.
    DeviceIdRegister = 0x8,
    // Implementation-assigned device vendor identifier.
    VendorIdRegister = 0xc,
    // Selected 32-bit word of offered feature bits.
    DeviceFeaturesRegister = 0x10,
    // Select the offered feature word.
    DeviceFeaturesSelectorRegister = 0x14,
    // Selected 32-bit word of accepted feature bits.
    DriverFeaturesRegister = 0x20,
    // Select the accepted feature word.
    DriverFeaturesSelectorRegister = 0x24,
    // Select the queue whose configuration registers are accessed.
    QueueSelectorRegister = 0x30,
    // Maximum descriptor count for the selected queue; zero if absent.
    QueueMaximumSizeRegister = 0x34,
    // Driver-selected descriptor count.
    QueueSizeRegister = 0x38,
    // Driver marks the selected queue available for use.
    QueueReadyRegister = 0x44,
    // Notify the device of available buffers on the written queue index.
    QueueNotifyRegister = 0x50,
    // Pending used-buffer/configuration interrupt causes.
    InterruptStatusRegister = 0x60,
    // Write one bits to acknowledge pending interrupt causes.
    InterruptAcknowledgeRegister = 0x64,
    // Device initialization/failure flags; writing zero resets the device.
    StatusRegister = 0x70,
    // Low 32 bits of the descriptor table guest physical address.
    QueueDescriptorLowRegister = 0x80,
    // High 32 bits of the descriptor table guest physical address.
    QueueDescriptorHighRegister = 0x84,
    // Low 32 bits of the split queue available-ring address.
    QueueDriverLowRegister = 0x90,
    // High 32 bits of the split queue available-ring address.
    QueueDriverHighRegister = 0x94,
    // Low 32 bits of the split queue used-ring address.
    QueueDeviceLowRegister = 0xa0,
    // High 32 bits of the split queue used-ring address.
    QueueDeviceHighRegister = 0xa4,
    // Generation counter for a consistent device configuration read.
    ConfigurationGenerationRegister = 0xfc,
    // Beginning of device-specific configuration, rather than transport state.
    DeviceConfigurationRegisters = 0x100,
};
// Virtio 1.2, 2.1 Device Status Field. Flags accumulate during initialization;
// DEVICE_NEEDS_RESET/FAILED prevent normal queue processing.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-110001
enum DeviceStatus : uint32_t {
    Acknowledge = 1,       // Driver has recognized the device.
    Driver = 2,            // Driver knows how to operate the device.
    DriverOk = 4,          // Driver setup is complete and device may consume buffers.
    FeaturesOk = 8,        // Driver completed feature negotiation.
    DeviceNeedsReset = 64, // Device encountered an error requiring reset.
    Failed = 128,          // Driver abandoned initialization/operation.
};
// Modern feature bit 32, VIRTIO_F_VERSION_1 (Virtio 1.2, section 6).
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-6600006
constexpr unsigned Version1FeatureBit = 32;
constexpr uint64_t Version1Feature = 1ull << Version1FeatureBit;
// MMIO MagicValue is 0x74726976: little-endian bytes 'v', 'i', 'r', 't'.
constexpr uint32_t MmioMagicValue = 0x74726976;
// MMIO Version=2 is the non-legacy transport, not the Virtio specification revision.
constexpr uint32_t ModernMmioVersion = 2;
// IMPLEMENTATION NOTE: PCEmu's retained producer identifier, numeric ASCII PCEM
// (50 43 45 4d in most-significant-byte order). Not an allocated PCI Vendor ID.
constexpr uint32_t PcemuVendorId = 0x5043454d;
// InterruptStatus bit 0 signals used buffers; bit 1 signals configuration change.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1670002
constexpr uint32_t UsedBufferInterrupt = 1, ConfigurationChangeInterrupt = 2;
// Split descriptor layout, Virtio 1.2, 2.7.5: 16-byte little-endian records.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-430005
enum DescriptorField : unsigned {
    Address = 0, // 64-bit guest physical buffer address.
    Length = 8,  // 32-bit buffer byte count.
    Flags = 12,  // 16-bit NEXT/WRITE/INDIRECT flags.
    Next = 14,   // 16-bit next descriptor index, meaningful with NEXT set.
};
constexpr unsigned DescriptorBytes = 16;
constexpr uint16_t DescriptorNext = 1;  // Another descriptor follows via Next.
constexpr uint16_t DescriptorWrite = 2; // Buffer is device-writable, not device-readable.
// Split rings start with flags:u16, idx:u16, then ring entries at offset 4.
// Available entries are descriptor indices:u16; used entries are id:u32,len:u32.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-490006
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-540008
constexpr unsigned RingFlagsOffset = 0, RingIndexOffset = 2, RingEntriesOffset = 4;
constexpr unsigned AvailableEntryBytes = 2, UsedEntryBytes = 8, UsedLengthOffset = 4;
// Include the optional trailing event-index:u16 in each ring's validated extent,
// even though this core does not offer VIRTIO_F_EVENT_IDX.
constexpr unsigned RingFixedBytesWithEvent = 6;
// Available flags bit 0 requests suppression of used-buffer notifications.
constexpr uint16_t AvailableNoInterrupt = 1;
} // namespace pc::virtio
