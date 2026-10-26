#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <utility>
namespace pc {
// ARM64 Synthetic Interrupt Controller (SynIC), virtual trust level 0 only.
// Microsoft Hypervisor Top-Level Functional Specification (TLFS), Inter-Partition
// Communication / HV_MESSAGE / HV_REGISTER_NAME (online ARM64 register names).
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#synic-registers
// All virtual processors (VPs) must be stopped, including during shared-RAM writes.
// The caller supplies actual Generic Interrupt Controller (GIC) delivery.
class ArmSynic {
  public:
    // ARM64 hypercall register identifiers, not x64 model-specific register addresses.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name#synthetic-interrupt-controller-synic--primary
    enum Register : uint32_t {
        SyntheticInterruptFirstRegister =
            0xa0000,                      // SINT0..15 select delivery/mask/polling per source.
        ControlRegister = 0xa0010,        // SCONTROL enables this VP's SynIC.
        VersionRegister = 0xa0011,        // SVERSION reports interface version 1, read-only.
        EventFlagsPageRegister = 0xa0012, // SIFP/SIEFP registers guest event-bit memory.
        MessagePageRegister = 0xa0013,    // SIPP/SIMP registers guest message-slot memory.
        EndOfMessageRegister = 0xa0014,   // EOM scans this VP's pending queues; reads zero.
    };
    // Sixteen independent synthetic sources, each with one 256-byte message slot
    // and 2048 event bits (256 bytes), together occupying one 4 KiB page of each kind.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#sim-page
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#sief-page
    static constexpr unsigned SyntheticInterruptCount = 16, MessageBytes = 256,
                              EventFlagsBytes = 256, EventFlagsCount = EventFlagsBytes * 8;
    // SINT.Masked starts set; bit 18 Polling allows publication without interrupt delivery.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#sintx-registers
    static constexpr uint64_t InterruptMasked = 1ull << 16, InterruptPolling = 1ull << 18;
    // IMPLEMENTATION NOTE: Bounded host storage, not a TLFS architectural VP limit.
    static constexpr unsigned MaximumVirtualProcessors = 8;
    using Interrupt = std::function<bool(uint64_t, unsigned)>;
    // Construct disabled SynIC state for a bounded nonempty set of VPs.
    ArmSynic(std::span<uint8_t> ram, uint64_t base, unsigned cpus);
    // Bind real GIC delivery; callback receives a VP bitmap and private interrupt ID.
    // IMPLEMENTATION NOTE: Host wiring supporting TLFS delivery, all VPs stopped.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#event-flag-delivery
    void set_interrupt_sender(Interrupt send) {
        interrupt = std::move(send);
    }
    // Rescan after a stopped execution slice: GIC EOI is handled natively
    // and need not cause a host exit. Occupied slots are never overwritten.
    uint16_t service_message_queues();
    // Inspect queued work on enabled message pages; does not publish or inject.
    bool needs_service() const;
    // Bounded metadata only; never exposes arbitrary message payloads.
    std::string report() const;
    // Read this VP's SynIC register; clear output and return an HV_STATUS on failure.
    uint16_t read_vp_register(uint32_t name, unsigned vp, uint64_t &value) const;
    // Validate/store a register, clear newly mapped pages or service EOM.
    uint16_t write_vp_register(uint32_t name, unsigned vp, uint64_t value);
    // Set a previously clear event bit and notify; rollback on failed GIC delivery.
    uint16_t signal_event(unsigned vp, unsigned sint, unsigned flag, bool &newlySignaled);
    // Dedicated timer buffers remain owned by the timer until a slot is free.
    uint16_t try_post_message(unsigned vp, unsigned sint, uint32_t type, uint64_t sender,
                              std::span<const uint8_t> payload);
    // Queue a bounded message and attempt immediate publication; rollback on failure.
    uint16_t post_message(unsigned vp, unsigned sint, uint32_t type, uint64_t sender,
                          std::span<const uint8_t> payload);

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    using Message = std::array<uint8_t, MessageBytes>;
    struct Vp {
        uint64_t control = 0, events = 0, messages = 0;
        std::array<uint64_t, SyntheticInterruptCount> sint;
        std::array<std::deque<Message>, SyntheticInterruptCount> queue;
        // Reset creates masked sources, disabled overlays and empty queues.
        // TLFS SINTx Registers.
        // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#sintx-registers
        Vp() {
            sint.fill(InterruptMasked);
        }
    };
    std::array<Vp, MaximumVirtualProcessors> state;
    std::span<uint8_t> ram;
    uint64_t base;
    unsigned cpus;
    Interrupt interrupt;
    // Validate enabled, page-aligned overlay registration wholly within host RAM.
    std::span<uint8_t> registered_page(uint64_t reg) const;
    // Honor polling/masking, otherwise request the configured private GIC interrupt.
    bool notify_synthetic_interrupt(unsigned vp, unsigned sint);
    // Publish one queued message into an empty slot and update MessagePending.
    uint16_t deliver_queued_message(unsigned vp, unsigned sint);
};
} // namespace pc
