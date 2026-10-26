#pragma once
#include "virtio_blk.hpp"
#include <array>
#include <functional>
#include <vector>
namespace pc {
// Modern virtio PCI endpoint with common/device/notify/ISR capabilities.
// INTx uses a 16 KiB BAR; an optional MSI sender enables MSI-X and a 32 KiB BAR.
class VirtioPciDevice {
  public:
    using Message = std::function<void(uint64_t, uint32_t)>;
    // Bind backend callbacks and build modern Virtio PCI capabilities and optional MSI-X state.
    VirtioPciDevice(VirtioBlock &block, std::function<void(bool)> interrupt, Message message = {});
    using Reader = std::function<uint64_t(unsigned, unsigned)>;
    using Writer = std::function<void(unsigned, uint64_t, unsigned)>;
    // Bind backend callbacks and build modern Virtio PCI capabilities and optional MSI-X state.
    VirtioPciDevice(Reader read, Writer write, uint16_t deviceId, uint32_t classCode,
                    uint16_t queues, std::function<void(bool)> interrupt, Message message = {});
    // Read PCI header/BAR probe state and live MSI-X control in little-endian byte order.
    uint64_t configRead(uint64_t offset, unsigned width) const;
    // Apply BAR/Command/routing writes and replay newly unmasked pending MSI-X messages.
    void configWrite(uint64_t offset, uint64_t value, unsigned width);
    // Test enabled BAR decoding inside the board PCI memory aperture.
    bool contains(uint64_t address, unsigned width) const;
    // Read the enabled BAR common/device/MSI-X regions; an ISR read acknowledges causes.
    uint64_t read(uint64_t address, unsigned width);
    // Write common/device/vector-table fields or notify a queue when bus mastering is enabled.
    void write(uint64_t address, uint64_t value, unsigned width);
    // Route backend causes through INTx or record/send MSI-X and acknowledge consumed causes.
    void irq(bool level, unsigned queue = 0);
    uint64_t messageInterrupts = 0;
    // Report whether PCI MSI-X Message Control.Enable is set.
    // IMPLEMENTATION NOTE: Host dispatch/inspection of the transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1270003
    bool msixEnabled() const {
        return (msixControl & MsixEnable) != 0;
    }

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    // PCI MSI-X Message Control enable[15] and function mask[14].
    // https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=238
    static constexpr uint16_t MsixEnable = 0x8000, MsixFunctionMask = 0x4000;
    // IMPLEMENTATION NOTE: Board BAR apertures with/without MSI-X regions.
    static constexpr unsigned IntxBarBytes = 0x4000, MsixBarBytes = 0x8000;
    // Conventional PCI header byte count and MSI-X table/PBA storage geometry.
    static constexpr unsigned ConfigurationHeaderBytes = 256, VectorEntryBytes = 16,
                              PendingBitArrayBytes = 32;
    // Virtio's 0xffff sentinel disables an MSI-X vector assignment.
    static constexpr uint16_t NoMsixVector = 0xffff;
    Reader reader;
    Writer writer;
    uint16_t queueCount;
    // Read the underlying modern MMIO register interface.
    // IMPLEMENTATION NOTE: Host dispatch/inspection of the transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1270003
    uint64_t read_backend_register(unsigned offset, unsigned width = 4) const {
        return reader(offset, width);
    }
    // Write the underlying modern MMIO register interface.
    // IMPLEMENTATION NOTE: Host dispatch/inspection of the transport state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1270003
    void write_backend_register(unsigned offset, uint64_t value, unsigned width = 4) {
        writer(offset, value, width);
    }
    std::function<void(bool)> interrupt;
    std::array<uint8_t, ConfigurationHeaderBytes> config{};
    uint32_t bar = 0;
    bool barProbe = false, irqLevel = false;
    uint16_t command = 0;
    Message message;
    uint16_t msixControl = 0, configVector = NoMsixVector;
    std::vector<uint16_t> queueVectors;
    std::vector<std::array<uint8_t, VectorEntryBytes>> vectorTable;
    std::array<uint8_t, PendingBitArrayBytes> pendingBits{};
    // IMPLEMENTATION NOTE: Size includes MSI-X table/PBA only with a host message sender.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-1240004
    unsigned bar_size() const {
        return message ? MsixBarBytes : IntxBarBytes;
    }
    // Set a vector’s PBA bit and attempt delivery; ignore absent/unassigned vectors.
    void mark_vector_pending(uint16_t vector);
    // Send a pending unmasked MSI-X message when enabled and bus mastering is allowed.
    void deliver_pending_vector(unsigned vector);
    // Retry every pending vector after mask/control changes.
    void deliver_pending_vectors();
    // Drive the level INTx callback unless PCI Command or MSI-X disables that pin.
    void update_intx();
    // Synthesize the common configuration fields from live backend state and vector assignments.
    uint64_t read_common_configuration(unsigned offset, unsigned width) const;
    // Translate writable common fields to backend MMIO and retain PCI MSI-X assignments.
    void write_common_configuration(unsigned offset, uint64_t value, unsigned width);
};
// Existing block-device call sites retain their descriptive name.
using VirtioPciBlock = VirtioPciDevice;
} // namespace pc
