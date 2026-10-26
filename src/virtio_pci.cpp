// Modern Virtio PCI transport adapts PCI discovery/BAR accesses to a backend
// implementing the Virtio MMIO register interface. Vendor-specific capabilities
// locate common configuration, queue notifications, interrupt status and device
// configuration in BAR0. Common fields translate to their MMIO counterparts;
// queue_notify_off uses queue index with a four-byte multiplier. The endpoint
// owns BAR relocation/size probes and PCI Command decoding; the ECAM bus routes it.
//
// Without a host message callback, completion uses the level-triggered INTx pin.
// With one, MSI-X adds a per-queue vector plus a configuration vector, a message
// address/data table and Pending Bit Array (PBA). Masked vectors remain pending;
// unmasking/enabling/bus-master changes replay them. MSI-X records pending causes
// before acknowledging backend latches, allowing subsequent completions to signal.
// Reading ISR clears its backend causes, as required by the Virtio PCI interface.
//
// IMPLEMENTATION NOTE: BAR0 occupies 16 KiB (INTx) or 32 KiB (MSI-X); its layout,
// capability positions and allowed board MMIO aperture are host choices. There
// are at most 256 vectors. Message callback performs host-specific interrupt delivery.
// No legacy Virtio interface or optional shared memory is exposed. FIXME: Add
// the required PCI configuration access capability before claiming all Virtio 1.2
// PCI device requirements; this bounded capability set is retained for compatibility.
// Virtio 1.2, 4.1 PCI transport; PCI Local Bus 3.0, 6.8.2 MSI-X.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1240004
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1270003
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=238

#include "virtio_pci.hpp"
#include "virtio_registers.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
namespace {
// PCI type-0 header byte offsets (PCI Local Bus 3.0, 6.1/6.2).
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=214
enum ConfigurationRegister : unsigned {
    // Vendor/device identification (two little-endian u16 values).
    VendorDeviceIdRegister = 0,
    // PCI I/O/memory/bus-master enables and INTx disable (u16).
    CommandRegister = 4,
    // Capabilities-list and interrupt-pending status (u16).
    StatusRegister = 6,
    // Revision byte followed by programming interface/subclass/base class.
    ClassRevisionRegister = 8,
    // Guest-stored cache-line-size byte.
    CacheLineSizeRegister = 0x0c,
    // Guest-stored latency-timer byte.
    LatencyTimerRegister = 0x0d,
    // BAR0 base address or size-probe response (u32).
    BaseAddressRegister = 0x10,
    // Subsystem vendor/device identity (two u16 values).
    SubsystemIdRegister = 0x2c,
    // Byte offset of the first capability in this function.
    CapabilitiesPointerRegister = 0x34,
    // Platform routing byte; starts unassigned (ffh).
    InterruptLineRegister = 0x3c,
    // INTA# pin number (1).
    InterruptPinRegister = 0x3d,

};
// Virtio 1.2, 4.1.2 PCI discovery: vendor 1af4h, modern device ID 1040h+type.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1170002
constexpr uint16_t VirtioVendorId = 0x1af4, ModernDeviceIdBase = 0x1040;
// IMPLEMENTATION NOTE: Retained subsystem IDs are 40h+device type, vendor 1af4h.
constexpr uint16_t SubsystemDeviceIdBase = 0x40;
// PCI Status: capabilities-list present bit 4 and pending INTx bit 3.
constexpr uint16_t CapabilitiesListPresent = 0x10, InterruptPending = 8;
// PCI Command: I/O, memory and bus-master requests occupy bits [2:0]; bit 10
// disables INTx. Only memory enables BAR decoding; bus master gates notifications/MSI.
// Writable Command bytes retain bits [2:0] and Interrupt Disable[10] only.
constexpr uint8_t CommandLowWritableMask = 7, CommandHighWritableMask = 4;
constexpr uint16_t LowByteMask = 0xff, HighByteMask = 0xff00;
// Interrupt Pin value 1 selects INTA#; Interrupt Line 0xff is the unassigned sentinel.
constexpr uint8_t InterruptPinA = 1;
constexpr uint16_t MemorySpaceEnable = 2, BusMasterEnable = 4, InterruptDisable = 0x400;
// All ones requests BAR size discovery; low alignment bits remain hardwired zero.
constexpr uint32_t BarSizeProbeRequest = 0xffffffffu;
// IMPLEMENTATION NOTE: Endpoint BARs must lie in this board's PCI MMIO aperture.
constexpr uint32_t BoardMemoryWindowBegin = 0x10000000, BoardMemoryWindowEnd = 0x3f000000;
// Conventional config is 256 bytes; ECAM addresses a full 4096-byte function.
constexpr unsigned ConventionalConfigurationBytes = 256, FunctionConfigurationBytes = 4096;
constexpr uint64_t FunctionOffsetMask = FunctionConfigurationBytes - 1;
// Virtio vendor capability location/stride are implementation choices; cap layout
// is Virtio 1.2, 4.1.4. Each capability points into BAR0 at a separate 4 KiB page.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1240004
constexpr unsigned FirstCapabilityOffset = 0x40, CapabilityStride = 0x14, VirtioCapabilityCount = 4,
                   VendorSpecificCapabilityId = 9;
enum CapabilityField : unsigned {
    Next = 1,
    Length = 2,
    ConfigurationType = 3,
    BarIndex = 4,
    BarOffset = 8,
    RegionLength = 12,
    NotifyOffsetMultiplier = 16,
};
constexpr unsigned StandardCapabilityBytes = 16, NotificationCapabilityBytes = 20;
// Virtio cfg_type numbers are common=1, notify=2, ISR=3, device=4.
constexpr unsigned CommonCapabilityIndex = 0, NotifyCapabilityIndex = 1, IsrCapabilityIndex = 2,
                   DeviceCapabilityIndex = 3;
// IMPLEMENTATION NOTE: Chosen BAR0 layout; capabilities publish these locations.
enum BarRegion : unsigned {
    CommonConfigurationRegion = 0,
    NotificationRegion = 0x1000,
    InterruptStatusRegion = 0x2000,
    DeviceConfigurationRegion = 0x3000,
    MsixTableRegion = 0x4000,
    MsixPendingRegion = 0x5000,
};
constexpr unsigned CapabilityRegionStride = 0x1000, DeviceConfigurationBytes = 64;
// queue_notify_off is the queue index; multiplier 4 spaces notifications four bytes apart.
constexpr unsigned NotificationOffsetMultiplier = 4;
// Common configuration offsets, Virtio 1.2, 4.1.4.3.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1270003
enum CommonField : unsigned {
    // Select offered feature word (u32).
    DeviceFeatureSelector = 0,
    // Read the offered feature word (u32).
    DeviceFeatures = 4,
    // Select accepted feature word (u32).
    DriverFeatureSelector = 8,
    // Write the accepted feature word (u32).
    DriverFeatures = 12,
    // MSI-X vector for configuration notifications (u16).
    ConfigurationMsixVector = 16,
    // Number of implemented queues (u16).
    NumberOfQueues = 18,
    // Initialization/failure flags (u8).
    DeviceStatus = 20,
    // Configuration consistency generation (u8).
    ConfigurationGeneration = 21,
    // Select a queue for subsequent queue fields (u16).
    QueueSelector = 22,
    // Selected queue descriptor count (u16).
    QueueSize = 24,
    // Selected queue completion vector or NO_VECTOR (u16).
    QueueMsixVector = 26,
    // Selected queue ready flag (u16).
    QueueEnable = 28,
    // Selected queue notification-region offset unit (u16).
    QueueNotifyOffset = 30,
    // Descriptor table guest physical address (u64).
    QueueDescriptorAddress = 32,
    // Available ring guest physical address (u64).
    QueueDriverAddress = 40,
    // Used ring guest physical address (u64).
    QueueDeviceAddress = 48,

};
constexpr unsigned CommonConfigurationBytes = 56, QueueAddressCount = 3, QueueAddressBytes = 8,
                   MmioQueueAddressStride = 0x10;
// MSI-X capability and table/PBA structure, PCI Local Bus 3.0, 6.8.2.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=238
constexpr unsigned MsixCapabilityOffset = 0x90, MsixCapabilityId = 0x11, MsixControlOffset = 0x92,
                   MsixTableOffset = 0x94, MsixPendingOffset = 0x98;
constexpr unsigned MsixEntryBytes = 16, MsixDataOffset = 8, MsixVectorControlOffset = 12;
constexpr uint8_t MsixVectorMask = 1; // Per-entry Vector Control bit 0 masks delivery.
// IMPLEMENTATION NOTE: 256 vectors fit the fixed 32-byte Pending Bit Array (PBA).
// One vector is reserved for configuration, leaving at most 255 queue vectors.
constexpr unsigned MaximumQueueVectors = 255, PendingArrayBytes = 32;
constexpr uint16_t NoVector = 0xffff; // VIRTIO_MSI_NO_VECTOR: disable interrupt assignment.
constexpr unsigned BitsPerByte = 8, WordBits = 32;
// Block convenience constructor: Virtio block type 2, class storage/SCSI 010000h,
// revision 1. The class/revision value is board identity, not an MMIO register.
constexpr uint16_t BlockDeviceId = 2;
constexpr uint32_t BlockClassRevision = 0x01000001;
// Store a little-endian PCI field into the host configuration scratch array.
// PCI 3.0 configuration header; Virtio 1.2 capability/common field layout.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1240004
void write_little_endian_field(std::array<uint8_t, 256> &bytes, unsigned offset, uint64_t value,
                               unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes[offset + i] = uint8_t(value >> (BitsPerByte * i));
}
// Return ones across the requested byte width for an unimplemented read.
// PCI 3.0 configuration header; Virtio 1.2 capability/common field layout.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1240004
uint64_t access_value_mask(unsigned width) {
    return width == 8 ? UINT64_MAX : (1ull << (BitsPerByte * width)) - 1;
}
// Reject unsupported widths, misalignment and region-boundary overflow.
// PCI 3.0 configuration header; Virtio 1.2 capability/common field layout.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1240004
void validate_access(unsigned offset, unsigned width, unsigned size) {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset % width || offset > size ||
        width > size - offset)
        throw std::runtime_error("Invalid virtio PCI access");
}
} // namespace
// Bind backend callbacks and build modern Virtio PCI capabilities and optional MSI-X state.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1240004
VirtioPciDevice::VirtioPciDevice(VirtioBlock &b, std::function<void(bool)> irq, Message msi)
    : VirtioPciDevice([&b](unsigned o, unsigned w) { return b.read(o, w); },
                      [&b](unsigned o, uint64_t v, unsigned w) { b.write(o, v, w); }, BlockDeviceId,
                      BlockClassRevision, 1, std::move(irq), std::move(msi)) {}
// Bind backend callbacks and build modern Virtio PCI capabilities and optional MSI-X state.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1240004
VirtioPciDevice::VirtioPciDevice(Reader read, Writer write, uint16_t deviceId, uint32_t classCode,
                                 uint16_t queues, std::function<void(bool)> irq, Message msi)
    : reader(std::move(read)), writer(std::move(write)), queueCount(queues),
      interrupt(std::move(irq)), message(std::move(msi)), queueVectors(queues, NoVector),
      vectorTable(message ? queues + 1 : 0) {
    if (message && queues > MaximumQueueVectors)
        throw std::runtime_error("Too many MSI-X vectors");
    for (auto &entry : vectorTable)
        entry[MsixVectorControlOffset] = MsixVectorMask;
    write_backend_register(virtio::Register::QueueSizeRegister,
                           read_backend_register(virtio::Register::QueueMaximumSizeRegister));
    write_little_endian_field(config, ConfigurationRegister::VendorDeviceIdRegister,
                              (uint32_t(ModernDeviceIdBase + deviceId) << 16) | VirtioVendorId, 4);
    write_little_endian_field(config, ConfigurationRegister::StatusRegister,
                              CapabilitiesListPresent, 2); // capabilities list
    write_little_endian_field(config, ConfigurationRegister::ClassRevisionRegister, classCode,
                              4); // mass storage/SCSI, revision 1
    write_little_endian_field(config, ConfigurationRegister::SubsystemIdRegister,
                              (uint32_t(SubsystemDeviceIdBase + deviceId) << 16) | VirtioVendorId,
                              4);
    config[ConfigurationRegister::CapabilitiesPointerRegister] = FirstCapabilityOffset;
    config[ConfigurationRegister::InterruptLineRegister] = UINT8_MAX;
    config[ConfigurationRegister::InterruptPinRegister] = InterruptPinA;
    for (unsigned i = 0; i < VirtioCapabilityCount; ++i) {
        unsigned o = FirstCapabilityOffset + i * CapabilityStride;
        config[o] = VendorSpecificCapabilityId; // vendor-specific capability
        config[o + CapabilityField::Next] = i == DeviceCapabilityIndex
                                                ? (message ? MsixCapabilityOffset : 0)
                                                : o + CapabilityStride;
        config[o + CapabilityField::Length] =
            i == NotifyCapabilityIndex ? NotificationCapabilityBytes : StandardCapabilityBytes;
        config[o + CapabilityField::ConfigurationType] = i + 1; // common, notify, ISR, device
        config[o + CapabilityField::BarIndex] = 0;              // BAR0
        write_little_endian_field(config, o + CapabilityField::BarOffset,
                                  i * CapabilityRegionStride, 4);
        write_little_endian_field(config, o + CapabilityField::RegionLength,
                                  i == CommonCapabilityIndex   ? CommonConfigurationBytes
                                  : i == NotifyCapabilityIndex ? CapabilityRegionStride
                                  : i == IsrCapabilityIndex    ? 1
                                                               : DeviceConfigurationBytes,
                                  4);
        if (i == NotifyCapabilityIndex)
            write_little_endian_field(config, o + CapabilityField::NotifyOffsetMultiplier,
                                      NotificationOffsetMultiplier, 4); // notify_off_multiplier
    }
    if (message) {
        config[MsixCapabilityOffset] = MsixCapabilityId; // PCI MSI-X capability
        write_little_endian_field(config, MsixTableOffset, BarRegion::MsixTableRegion,
                                  4); // table in BAR0
        write_little_endian_field(config, MsixPendingOffset, BarRegion::MsixPendingRegion,
                                  4); // pending bit array in BAR0
    }
}
// Read PCI header/BAR probe state and live MSI-X control in little-endian byte order.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=238
uint64_t VirtioPciDevice::configRead(uint64_t offset, unsigned width) const {
    validate_access(offset & FunctionOffsetMask, width, FunctionConfigurationBytes);
    if (offset >= ConventionalConfigurationBytes)
        return access_value_mask(width); // Extended configuration bytes are unimplemented.
    auto bytes = config;
    write_little_endian_field(bytes, ConfigurationRegister::CommandRegister, command, 2);
    write_little_endian_field(bytes, ConfigurationRegister::StatusRegister,
                              CapabilitiesListPresent | (irqLevel ? InterruptPending : 0), 2);
    write_little_endian_field(bytes, ConfigurationRegister::BaseAddressRegister,
                              barProbe ? uint32_t(~(bar_size() - 1)) : bar, 4);
    if (message)
        // Table Size is encoded as vector count minus one. One config vector plus
        // queueCount queue vectors therefore encodes queueCount in bits [10:0].
        write_little_endian_field(bytes, MsixControlOffset, queueCount | msixControl, 2);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(bytes[offset + i]) << (BitsPerByte * i);
    return value;
}
// Apply BAR/Command/routing writes and replay newly unmasked pending MSI-X messages.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=238
void VirtioPciDevice::configWrite(uint64_t offset, uint64_t value, unsigned width) {
    validate_access(offset & FunctionOffsetMask, width, FunctionConfigurationBytes);
    if (offset >= ConventionalConfigurationBytes)
        return;
    if (offset == ConfigurationRegister::BaseAddressRegister && width == 4) {
        barProbe = uint32_t(value) == BarSizeProbeRequest;
        if (!barProbe)
            bar = uint32_t(value) & uint32_t(~(bar_size() - 1));
        return;
    }
    for (unsigned i = 0; i < width; ++i) {
        auto at = offset + i;
        uint8_t byte = value >> (i * BitsPerByte);
        if (at == ConfigurationRegister::CommandRegister)
            command = (command & HighByteMask) | (byte & CommandLowWritableMask);
        if (at == ConfigurationRegister::CommandRegister + 1)
            command = (command & LowByteMask) | (uint16_t(byte & CommandHighWritableMask) << 8);
        if (at == ConfigurationRegister::CacheLineSizeRegister ||
            at == ConfigurationRegister::LatencyTimerRegister ||
            at == ConfigurationRegister::InterruptLineRegister)
            config[at] = byte;
        if (message && at == MsixControlOffset + 1)
            msixControl = uint16_t(byte & ((MsixEnable | MsixFunctionMask) >> BitsPerByte)) << 8;
    }
    update_intx();
    if (message) {
        if ((msixControl & MsixEnable) && irqLevel)
            irq(true);
        deliver_pending_vectors();
    }
}
// Test enabled BAR decoding inside the board PCI memory aperture.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1240004
bool VirtioPciDevice::contains(uint64_t address, unsigned width) const {
    return (command & MemorySpaceEnable) && bar >= BoardMemoryWindowBegin &&
           bar < BoardMemoryWindowEnd && address >= bar && address - bar < bar_size() &&
           width <= bar_size() - (address - bar);
}
// Route backend causes through INTx or record/send MSI-X and acknowledge consumed causes.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1320005
void VirtioPciDevice::irq(bool level, unsigned queue) {
    irqLevel = level;
    update_intx();
    if (level && (msixControl & MsixEnable)) {
        auto causes = read_backend_register(virtio::Register::InterruptStatusRegister);
        if (causes & virtio::ConfigurationChangeInterrupt)
            mark_vector_pending(configVector);
        if ((causes & virtio::UsedBufferInterrupt) && queue < queueVectors.size())
            mark_vector_pending(queueVectors[queue]);
        // MSI-X is edge based. Retire the backend latch after recording masked
        // messages in the PBA, so later completions can signal independently.
        if (causes)
            write_backend_register(virtio::Register::InterruptAcknowledgeRegister, causes);
    }
}
// Drive the level INTx callback unless PCI Command or MSI-X disables that pin.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=238
void VirtioPciDevice::update_intx() {
    interrupt(irqLevel && !(command & InterruptDisable) && !(msixControl & MsixEnable));
}
// Set a vector’s PBA bit and attempt delivery; ignore absent/unassigned vectors.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=238
void VirtioPciDevice::mark_vector_pending(uint16_t vector) {
    if (vector >= vectorTable.size())
        return;
    pendingBits[vector / BitsPerByte] |= uint8_t(1u << (vector % BitsPerByte));
    deliver_pending_vector(vector);
}
// Send a pending unmasked MSI-X message when enabled and bus mastering is allowed.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=238
void VirtioPciDevice::deliver_pending_vector(unsigned vector) {
    auto &entry = vectorTable[vector];
    auto bit = uint8_t(1u << (vector % BitsPerByte));
    if (!(msixControl & MsixEnable) || (msixControl & MsixFunctionMask) ||
        !(command & BusMasterEnable) || (entry[MsixVectorControlOffset] & MsixVectorMask) ||
        !(pendingBits[vector / BitsPerByte] & bit))
        return;
    uint64_t address = 0;
    uint32_t data = 0;
    for (unsigned i = 0; i < 8; ++i)
        address |= uint64_t(entry[i]) << (i * BitsPerByte);
    for (unsigned i = 0; i < 4; ++i)
        data |= uint32_t(entry[MsixDataOffset + i]) << (i * BitsPerByte);
    message(address, data);
    pendingBits[vector / BitsPerByte] &= uint8_t(~bit);
    ++messageInterrupts;
}
// Retry every pending vector after mask/control changes.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=238
void VirtioPciDevice::deliver_pending_vectors() {
    for (unsigned i = 0; i < vectorTable.size(); ++i)
        deliver_pending_vector(i);
}
// Synthesize the common configuration fields from live backend state and vector assignments.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1270003
uint64_t VirtioPciDevice::read_common_configuration(unsigned offset, unsigned width) const {
    validate_access(offset, width, CommonConfigurationBytes);
    std::array<uint8_t, 256> common{};
    write_little_endian_field(
        common, CommonField::DeviceFeatureSelector,
        read_backend_register(virtio::Register::DeviceFeaturesSelectorRegister), 4);
    write_little_endian_field(common, CommonField::DeviceFeatures,
                              read_backend_register(virtio::Register::DeviceFeaturesRegister), 4);
    write_little_endian_field(
        common, CommonField::DriverFeatureSelector,
        read_backend_register(virtio::Register::DriverFeaturesSelectorRegister), 4);
    write_little_endian_field(common, CommonField::DriverFeatures,
                              read_backend_register(virtio::Register::DriverFeaturesRegister), 4);
    write_little_endian_field(common, CommonField::ConfigurationMsixVector, configVector, 2);
    write_little_endian_field(common, CommonField::NumberOfQueues, queueCount, 2);
    write_little_endian_field(common, CommonField::DeviceStatus,
                              read_backend_register(virtio::Register::StatusRegister), 1);
    write_little_endian_field(
        common, CommonField::ConfigurationGeneration,
        read_backend_register(virtio::Register::ConfigurationGenerationRegister), 1);
    write_little_endian_field(common, CommonField::QueueSelector,
                              read_backend_register(virtio::Register::QueueSelectorRegister), 2);
    write_little_endian_field(common, CommonField::QueueSize,
                              read_backend_register(virtio::Register::QueueSizeRegister), 2);
    write_little_endian_field(
        common, CommonField::QueueMsixVector,
        read_backend_register(virtio::Register::QueueSelectorRegister) < queueVectors.size()
            ? queueVectors[read_backend_register(virtio::Register::QueueSelectorRegister)]
            : NoVector,
        2);
    write_little_endian_field(common, CommonField::QueueEnable,
                              read_backend_register(virtio::Register::QueueReadyRegister), 2);
    write_little_endian_field(common, CommonField::QueueNotifyOffset,
                              read_backend_register(virtio::Register::QueueSelectorRegister), 2);
    for (unsigned n = 0; n < QueueAddressCount; ++n) {
        unsigned reg = virtio::Register::QueueDescriptorLowRegister + n * MmioQueueAddressStride;
        write_little_endian_field(
            common, CommonField::QueueDescriptorAddress + n * QueueAddressBytes,
            read_backend_register(reg) | (read_backend_register(reg + 4) << WordBits), 8);
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(common[offset + i]) << (i * BitsPerByte);
    return value;
}
// Translate writable common fields to backend MMIO and retain PCI MSI-X assignments.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1270003
void VirtioPciDevice::write_common_configuration(unsigned offset, uint64_t value, unsigned width) {
    validate_access(offset, width, CommonConfigurationBytes);
    if (offset == CommonField::DeviceFeatureSelector && width == 4)
        write_backend_register(virtio::Register::DeviceFeaturesSelectorRegister, value);
    else if (offset == CommonField::DriverFeatureSelector && width == 4)
        write_backend_register(virtio::Register::DriverFeaturesSelectorRegister, value);
    else if (offset == CommonField::DriverFeatures && width == 4)
        write_backend_register(virtio::Register::DriverFeaturesRegister, value);
    else if (offset == CommonField::DeviceStatus && width == 1) {
        write_backend_register(virtio::Register::StatusRegister, value);
        if (value == 0) {
            write_backend_register(
                virtio::Register::QueueSizeRegister,
                read_backend_register(virtio::Register::QueueMaximumSizeRegister));
            configVector = NoVector;
            std::fill(queueVectors.begin(), queueVectors.end(), NoVector);
            pendingBits.fill(0);
        }
    } else if (message && offset == CommonField::ConfigurationMsixVector && width == 2)
        configVector = value < vectorTable.size() ? uint16_t(value) : NoVector;
    else if (message && offset == CommonField::QueueMsixVector && width == 2) {
        auto queue = read_backend_register(virtio::Register::QueueSelectorRegister);
        if (queue < queueVectors.size())
            queueVectors[queue] = value < vectorTable.size() ? uint16_t(value) : NoVector;
    } else if (offset == CommonField::QueueSelector && width == 2)
        write_backend_register(virtio::Register::QueueSelectorRegister, value);
    else if (offset == CommonField::QueueSize && width == 2)
        write_backend_register(virtio::Register::QueueSizeRegister, value);
    else if (offset == CommonField::QueueEnable && width == 2)
        write_backend_register(virtio::Register::QueueReadyRegister, value);
    else if (offset >= CommonField::QueueDescriptorAddress && offset < CommonConfigurationBytes &&
             (width == 4 || width == 8)) {
        unsigned reg = virtio::Register::QueueDescriptorLowRegister +
                       ((offset - CommonField::QueueDescriptorAddress) / QueueAddressBytes) *
                           MmioQueueAddressStride +
                       ((offset - CommonField::QueueDescriptorAddress) % QueueAddressBytes);
        write_backend_register(reg, uint32_t(value));
        if (width == 8)
            write_backend_register(reg + 4, value >> WordBits);
    }
}
// Read the enabled BAR common/device/MSI-X regions; an ISR read acknowledges causes.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1320005
uint64_t VirtioPciDevice::read(uint64_t address, unsigned width) {
    if (!contains(address, width))
        throw std::runtime_error("Virtio PCI BAR is not enabled/mapped");
    unsigned offset = address - bar;
    validate_access(offset, width, bar_size());
    if (message && offset >= BarRegion::MsixTableRegion &&
        offset < BarRegion::MsixTableRegion + vectorTable.size() * MsixEntryBytes &&
        width <= BarRegion::MsixTableRegion + vectorTable.size() * MsixEntryBytes - offset) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) {
            auto at = offset + i - BarRegion::MsixTableRegion;
            value |= uint64_t(vectorTable[at / MsixEntryBytes][at % MsixEntryBytes])
                     << (i * BitsPerByte);
        }
        return value;
    }
    if (message && offset >= BarRegion::MsixPendingRegion &&
        offset < (BarRegion::MsixPendingRegion + PendingArrayBytes) &&
        width <= (BarRegion::MsixPendingRegion + PendingArrayBytes) - offset) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= uint64_t(pendingBits[offset - BarRegion::MsixPendingRegion + i])
                     << (i * BitsPerByte);
        return value;
    }
    if (offset < CommonConfigurationBytes)
        return read_common_configuration(offset, width);
    if (offset == BarRegion::InterruptStatusRegion && width == 1) {
        uint64_t value = read_backend_register(virtio::Register::InterruptStatusRegister);
        write_backend_register(virtio::Register::InterruptAcknowledgeRegister, value);
        return value;
    }
    if (offset >= BarRegion::DeviceConfigurationRegion &&
        offset < (BarRegion::DeviceConfigurationRegion + DeviceConfigurationBytes))
        return read_backend_register(virtio::Register::DeviceConfigurationRegisters + offset -
                                         BarRegion::DeviceConfigurationRegion,
                                     width);
    return 0;
}
// Write common/device/vector-table fields or notify a queue when bus mastering is enabled.
// Virtio 1.2 PCI transport / PCI 3.0 MSI-X; board bounds/layout are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1300004
void VirtioPciDevice::write(uint64_t address, uint64_t value, unsigned width) {
    if (!contains(address, width))
        throw std::runtime_error("Virtio PCI BAR is not enabled/mapped");
    unsigned offset = address - bar;
    validate_access(offset, width, bar_size());
    if (message && offset >= BarRegion::MsixTableRegion &&
        offset < BarRegion::MsixTableRegion + vectorTable.size() * MsixEntryBytes &&
        width <= BarRegion::MsixTableRegion + vectorTable.size() * MsixEntryBytes - offset) {
        for (unsigned i = 0; i < width; ++i) {
            auto at = offset + i - BarRegion::MsixTableRegion;
            auto byte = uint8_t(value >> (i * BitsPerByte));
            vectorTable[at / MsixEntryBytes][at % MsixEntryBytes] =
                at % MsixEntryBytes < MsixVectorControlOffset    ? byte
                : at % MsixEntryBytes == MsixVectorControlOffset ? byte & MsixVectorMask
                                                                 : 0;
        }
        deliver_pending_vectors();
    } else if (offset < CommonConfigurationBytes)
        write_common_configuration(offset, value, width);
    else if (offset >= BarRegion::NotificationRegion &&
             offset < BarRegion::NotificationRegion + queueCount * NotificationOffsetMultiplier &&
             (width == 2 || width == 4) && (command & BusMasterEnable))
        write_backend_register(virtio::Register::QueueNotifyRegister, value & UINT16_MAX);
    else if (offset >= BarRegion::DeviceConfigurationRegion &&
             offset < (BarRegion::DeviceConfigurationRegion + DeviceConfigurationBytes) &&
             width <= (BarRegion::DeviceConfigurationRegion + DeviceConfigurationBytes) - offset)
        write_backend_register(virtio::Register::DeviceConfigurationRegisters + offset -
                                   BarRegion::DeviceConfigurationRegion,
                               value, width);
}
} // namespace pc
