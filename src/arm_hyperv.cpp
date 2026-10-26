// This ARM64 Hyper-V subset lets a guest discover virtual-machine services and
// request them through HVC (Hypervisor Call) instructions. Discovery returns an
// interface UUID and feature registers; GuestOsId identifies the running OS.
// GetVpRegisters/SetVpRegisters transfer named 128-bit virtual-processor (VP)
// registers, either in aligned guest RAM or ARM register pairs. Repetition fields
// resume a partially completed register list and report the completed element count.
//
// The wrapper routes timer/SynIC (Synthetic Interrupt Controller) registers to
// their models. Timers publish messages or native GIC interrupts. Message/event
// connection IDs route guest PostMessage/SignalEvent requests to VMBus, whose
// shared rings carry heartbeat, clock synchronization, shutdown and key/value
// integration services. Host requests only queue work: guest acceptance of a
// shutdown packet is distinct from actual PSCI poweroff. The reference counter
// supplies partition-wide 100 ns time, optionally paired with a conversion page.
// Crash registers snapshot diagnostic parameters and optional guest RAM text.
//
// IMPLEMENTATION NOTE: All methods run with every VP stopped. The machine owns
// CPU exits, actual GIC interrupt injection, context/TLB synchronization and
// power actions; callbacks report whether those native operations succeeded.
// The model owns only bounded RAM validation, virtual register state and protocol
// routing. Native full-address-space flushes conservatively cover narrower guest
// requests. Polling intervals, clocks and diagnostic caps are host policy.
//
// This is not a complete Hyper-V implementation: only self-partition VTL0
// (Virtual Trust Level zero), at most eight VPs, fixed RAM-backed parameter pages
// and registered connection types are supported. No nested virtualization or
// extended capability services are advertised. Partition-info pages contain no
// asynchronous pending flushes; vector/TLBI registration alone enables no extra
// producer. Windows buffer-padding and legacy HVC #1 behavior are retained.
// Microsoft Hyper-V Top-Level Functional Specification (TLFS), online ARM64
// register/discovery reference and v6.0b status appendix; OpenVMM ARM ABI reference.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/feature-discovery
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/hv1/hvdef/src/lib.rs

#include "arm_hyperv.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>
namespace pc {
namespace {
// Microsoft TLFS, Feature and Interface Discovery (ARM64), and HV_REGISTER_NAME.
// Register numbers identify operations, not offsets into guest RAM.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name
namespace Register {
constexpr uint32_t HypervisorVersionRegister =
    0x100; // PCEmu build/version, not a Microsoft release.
constexpr uint32_t PrivilegesAndFeaturesInfoRegister = 0x200; // Which interfaces the guest may use.
constexpr uint32_t FeaturesInfoRegister = 0x201;              // Recommended guest optimizations.
constexpr uint32_t ImplementationLimitsInfoRegister =
    0x202;                                               // Configured VP/logical-processor bounds.
constexpr uint32_t HardwareFeaturesInfoRegister = 0x203; // Stage-2 translation availability.
constexpr uint32_t CpuManagementFeaturesInfoRegister = 0x204;
constexpr uint32_t PasidFeaturesInfoRegister = 0x205;
// COMPATIBILITY: These discovery slots return zero; no capabilities are claimed.
constexpr uint32_t ReservedFeaturesRegister = 0x206, NestedVirtFeaturesInfoRegister = 0x207;
constexpr uint32_t IptFeaturesInfoRegister = 0x208, IsolationConfigurationRegister = 0x209;
constexpr uint32_t GuestCrashP0Register = 0x210, GuestCrashP4Register = 0x214;
constexpr uint32_t GuestCrashControlRegister = 0x215; // Snapshot parameters and optional RAM text.
constexpr uint32_t GuestOsIdRegister = 0x90002; // Partition-wide identity written by the boot VP.
constexpr uint32_t VpIndexRegister = 0x90003;   // Calling VP's zero-based index.
constexpr uint32_t TimeReferenceCountRegister = 0x90004; // Partition time, in 100 ns units.
constexpr uint32_t InterfaceVersionRegister = 0x90006;   // Four-byte "Hv#1" interface signature.
constexpr uint32_t PartitionInfoPageRegister = 0x90015;  // Guest RAM page for pending-flush state.
constexpr uint32_t TlbiControlRegister = 0x90016;        // Per-VP architectural TLBI opt-in.
constexpr uint32_t ReferenceTscRegister =
    0x90017; // Reference-counter conversion page registration.
// Microsoft ARM guest register identifiers; SyntheticEsr is retained compatibility state.
// https://github.com/microsoft/mu_msvm/blob/main/MsvmPkg/Include/Hv/HvGuestHypercall.h#L1458
constexpr uint32_t SyntheticVectorBaseRegister = 0x40400; // Guest VA, independent of VBAR_EL1.
constexpr uint32_t SyntheticEsrRegister =
    0x40401; // No synthetic exception producers in this subset.
constexpr uint32_t Sint0Register = 0xa0000, SynicResendRegister = 0xa0015;
constexpr uint32_t Timer0ConfigurationRegister = 0xb0000, Timer3CountRegister = 0xb0007;
} // namespace Register
// TLFS Hypercall Reference: code selects an operation; bit 16 selects register input.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercalls/overview
namespace Hypercall {
constexpr uint16_t FlushVirtualAddressSpace = 0x2, SendSyntheticClusterIpi = 0xb;
constexpr uint16_t FlushVirtualAddressSpaceEx = 0x13, SendSyntheticClusterIpiEx = 0x15;
// COMPATIBILITY: ARM context synchronization/drain ABI is backed by native host callbacks.
// SyncContext layouts are Windows observations recorded in docs/hyperv-arm64.md.
// Microsoft guest ABI names 0xd6 HvCallFlushTlb (V7 ARM enlightened TLB flush).
// https://github.com/microsoft/mu_msvm/blob/main/MsvmPkg/Include/Hv/HvGuestHypercall.h#L638
constexpr uint16_t SyncContext = 0x19, SyncContextEx = 0x1a, FlushTlb = 0xd6;
constexpr uint16_t GetVpRegisters = 0x50, SetVpRegisters = 0x51;
constexpr uint16_t PostMessage = 0x5c, SignalEvent = 0x5d, QueryExtendedCapabilities = 0x8001;
} // namespace Hypercall
// TLFS Appendix A status codes. Return these before performing an invalid request.
// https://raw.githubusercontent.com/Microsoft/Virtualization-Documentation/master/tlfs/Hypervisor%20Top%20Level%20Functional%20Specification%20v6.0b.pdf#page=214
constexpr uint16_t InvalidHypercallCode = 2, InvalidHypercallInput = 3, InvalidAlignment = 4;
constexpr uint16_t InvalidParameter = 5, AccessDenied = 6, InvalidVpIndex = 14;
constexpr uint16_t InvalidPortId = 0x11, InvalidConnectionId = 0x12, OperationFailed = 0x71;
// SMCCC (SMC Calling Convention) vendor-hypervisor discovery and invocation IDs.
// COMPATIBILITY: Keep the UID call used by Linux/Windows. The current TLFS web
// prose spells a different discovery ID; do not silently substitute it here.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/feature-discovery
constexpr uint32_t VendorHypervisorUidFunction = 0x8600ff01, HyperVFunction = 0x46000001;
constexpr uint32_t SmcccVersionFunction = 0x80000000, SmcccArchFeaturesFunction = 0x80000001;
constexpr uint32_t SmcccVersion1_2 = 0x10002;
// The four SMCCC result words spell UUID 4d32ba58-cd24-4764-8eef-6c7516597024.
constexpr std::array<uint32_t, 4> HyperVInterfaceUid = {0x4d32ba58, 0xcd244764, 0x8eef6c75,
                                                        0x16597024};
constexpr uint32_t HyperVInterfaceSignature = 0x31237648; // Little-endian ASCII "Hv#1".
constexpr uint64_t PcEmuVersion = 1ull | (1ull << 48);    // Build 1, interface version 1.0.
// HV_PARTITION_PRIVILEGE_MASK: access reference count, timers, native interrupt
// controller, hypercalls, VP index, fast output. ARM native interrupts use GIC,
// the Generic Interrupt Controller, rather than x64 APIC EOI/ICR/TPR registers.
constexpr uint64_t AccessPartitionReferenceCounter = 1ull << 1;
constexpr uint64_t AccessSyntheticTimers = 1ull << 5, AccessInterruptController = 1ull << 6;
constexpr uint64_t AccessHypercall = 1ull << 49, AccessVpIndex = 1ull << 51,
                   EnableFastOutput = 1ull << 52;
constexpr uint64_t BasePrivileges = AccessPartitionReferenceCounter | AccessSyntheticTimers |
                                    AccessInterruptController | AccessHypercall | AccessVpIndex |
                                    EnableFastOutput;
constexpr uint64_t AccessSynic = 1ull << 2, AccessVmbusRegisters = 0x18;
constexpr uint64_t AccessReferenceTsc = 1ull << 9, PostMessagesPrivilege = 1ull << 36;
constexpr uint64_t SignalEventsPrivilege = 1ull << 37, CrashRegistersFeatureHigh = 1ull << 8;
constexpr uint64_t RecommendClusterIpi = 4, NoSpinWaitRecommendation = 0xffffffff00000000ull;
constexpr uint64_t SecondLevelAddressTranslation = 2;
// Hypercall control: fixed code bits [15:0], Fast [16], optional variable-header
// qword count [26:17], repetition count [43:32] and resume index [59:48].
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
constexpr uint64_t FastHypercall = 0x10000, SimpleControlMask = 0x1ffff;
constexpr uint64_t VariableHeaderControlMask = 0x7ffffff,
                   RegisterControlMask = 0x0fff0fff0001ffffull;
constexpr unsigned VariableHeaderShift = 17, RepCountShift = 32, RepStartShift = 48;
constexpr uint64_t VariableHeaderCountMask = 0x3ff, RepCountMask = 0xfff;
constexpr size_t HypercallPageBytes = 4096, PageOffsetMask = HypercallPageBytes - 1;
constexpr size_t ParameterAlignmentMask = 7, FastInputBytes = 128, RegisterPairBytes = 16;
constexpr size_t RegisterHeaderBytes = 16, RegisterAssociationBytes = 32, RegisterNameBytes = 4;
// Get/SetVpRegisters input: partition u64, VP index u32, VTL/reserved u32.
// Each Set association is name u32 + twelve padding bytes + a 128-bit value.
namespace RegisterInputField {
constexpr size_t PartitionId = 0, VpIndex = 8, VtlAndReserved = 12;
constexpr size_t NamePadding = 4, AlignmentPadding = 8, ValueLow = 16, ValueHigh = 24;
} // namespace RegisterInputField
// Timer-expired message: timer index u32, reserved u32, expiration/delivery u64.
namespace TimerMessageField {
constexpr size_t TimerIndex = 0, Expiration = 8, DeliveryTime = 16;
}
constexpr uint64_t SelfPartitionId = UINT64_MAX;
constexpr uint32_t SelfVpIndex = 0xfffffffe;
// Connection IDs are unscoped 24-bit IDs; scope bits are unsupported here.
constexpr uint32_t ConnectionIdMask = 0xffffff, VmbusControlConnection = 1,
                   VmbusBootstrapConnection = 4;
constexpr uint32_t VmbusMessageType = 1, HypervisorMessageTypeBit = 0x80000000;
constexpr size_t PostMessageBytes = 256, PostMessagePayloadBytes = 240, SignalEventBytes = 8;
namespace MessageField {
constexpr size_t Connection = 0, Reserved = 4, Type = 8, Size = 12, Payload = 16;
}
namespace EventField {
constexpr size_t Flag = 4, Reserved = 6;
}
// Crash control [63] notifies; [62] supplies a message at P3 with byte length P4.
// COMPATIBILITY: Bits [61:58] are retained; lower bits are rejected as before.
constexpr uint64_t CrashNotify = 1ull << 63, CrashMessage = 1ull << 62;
constexpr uint64_t CrashReservedLowMask = (1ull << 58) - 1;
// Page registration uses bit 0 to enable and bits [63:12] for its GPA; bits [11:1]
// must be zero. Synthetic exception vectors require separate 2 KiB VA alignment.
constexpr uint64_t PageEnabled = 1, PageReservedMask = 0xffe;
constexpr uint64_t TlbiEnabled = 1, SyntheticVectorAlignmentMask = 0x7ff;
// Flush flags: all processors, all address spaces and non-global mappings only.
// Native invalidation may conservatively include more translations.
constexpr uint64_t AllProcessors = 1, FlushFlagsMask = 7;
// IPI input packs SGI ID in low 32 bits and target-VTL flags in bits [36:32].
// Only current/explicit VTL0 is accepted; other bits are reserved.
constexpr uint64_t IpiFlagsMask = 0x1fffffffffull, UseTargetVtl = 0x10ull << 32,
                   TargetVtlMask = 0xfull << 32;
constexpr unsigned ArmSoftwareInterruptCount = 16; // GIC software-generated INTIDs 0..15.
constexpr uint64_t SparseVpSet = 0, AllVpSet = 1;
constexpr size_t VpSetHeaderBytes = 16, VpBankBytes = 8, MaximumVpBanks = 64;
// Timer Config: DirectMode selects native INTID [8:4]; otherwise SINT [19:16].
constexpr uint64_t TimerDirectMode = 0x1000;
constexpr unsigned TimerVectorShift = 4, TimerVectorMask = 31, TimerSintShift = 16,
                   TimerSintMask = 15;
constexpr uint32_t TimerExpiredMessage = 0x80000010;
constexpr size_t TimerMessageBytes = 24;
// IMPLEMENTATION NOTE: Bounded host storage/diagnostics, not advertised hardware limits.
constexpr size_t MaximumConnections = 64, MaximumDiagnosticEntries = 128;
constexpr unsigned MaximumVps = 8, MaximumEventFlags = 65536;
constexpr size_t MaximumCrashMessageBytes = 4096, MaximumControlPrefixBytes = 56;
constexpr uint64_t ReferenceTicksPerSecond = 10000000, NanosecondsPerReferenceTick = 100;
constexpr uint64_t MinimumServiceDelay100ns =
    10000; // Poll queued/busy work at most once per millisecond.
// Decode a validated little-endian wire field without alignment-dependent host loads.
// TLFS Hypercall Interface, natural alignment and parameter layouts.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
uint64_t read_little_endian(std::span<const uint8_t> b, size_t at, size_t size) {
    uint64_t v = 0;
    for (size_t i = 0; i < size; ++i)
        v |= uint64_t(b[at + i]) << (8 * i);
    return v;
}
// Encode a validated little-endian wire field or zero-padded synthetic message value.
// TLFS Hypercall Interface, parameter layouts.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
void write_little_endian(std::span<uint8_t> b, size_t at, uint64_t v, size_t size) {
    for (size_t i = 0; i < size; ++i)
        b[at + i] = uint8_t(v >> (8 * i));
}
} // namespace
// Construct initial register state, clocks and timer delivery routing; reject unsupported VP
// counts. Microsoft TLFS, Timers: Reference Counter and Synthetic Timers.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-counter
ArmHyperV::ArmHyperV(bool on, std::span<uint8_t> memory, uint64_t start, unsigned count,
                     Clock source)
    : enabled(on), synic(memory, start, count), vmbus(memory, start, count),
      referenceClock(memory, start), timers(count), ram(memory), base(start), cpus(count),
      clock(std::move(source)) {
    if (!clock)
        clock = [] {
            return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count()) /
                   NanosecondsPerReferenceTick;
        };
    clockOrigin = clock();
    timers.setDelivery([this](unsigned vp, unsigned timer, uint64_t config, uint64_t expiration,
                              uint64_t now) {
        if (config & TimerDirectMode)
            return privateInterruptSender &&
                   privateInterruptSender(1ull << vp,
                                          unsigned((config >> TimerVectorShift) & TimerVectorMask));
        std::array<uint8_t, TimerMessageBytes> payload{};
        write_little_endian(payload, TimerMessageField::TimerIndex, timer, 4);
        write_little_endian(payload, TimerMessageField::Expiration, expiration, 8);
        write_little_endian(payload, TimerMessageField::DeliveryTime, now, 8);
        return synic.try_post_message(vp, unsigned((config >> TimerSintShift) & TimerSintMask),
                                      TimerExpiredMessage, 0, payload) == 0;
    });
    if (!cpus || cpus > MaximumVps)
        throw std::runtime_error("Invalid Hyper-V VP count");
}
// Install a host counter faster than 10 MHz before guest identity; reject repeated/late setup.
// Microsoft TLFS, Timers: Reference Counter and Synthetic Timers.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-counter
bool ArmHyperV::enable_reference_clock(CounterProvider provider) {
    if (!enabled || guestId || counterProvider || !provider ||
        provider().frequency <= ReferenceTicksPerSecond)
        return false;
    counterProvider = std::move(provider);
    return true;
}
// Pair a host architectural counter/frequency with current partition reference time.
// Microsoft TLFS, Timers: Reference Counter and Synthetic Timers.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-counter
ArmHyperVReferenceClock::Sample ArmHyperV::reference_sample() const {
    auto sample = counterProvider();
    return {sample.value, reference_time_100ns(), sample.frequency};
}
// Invalidate before a host counter discontinuity, then republish any registered reference page.
// Microsoft TLFS, Timers: Reference Counter and Synthetic Timers.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-counter
void ArmHyperV::rebase_reference_clock(bool before) {
    if (!counterProvider)
        return;
    if (before)
        referenceClock.invalidate_reference_page();
    else if (referenceClock.registration() & PageEnabled)
        referenceClock.update_reference_mapping(reference_sample());
}
// Sample elapsed monotonic 100 ns time without moving behind the last reference-register read.
// Microsoft TLFS, Timers: Reference Counter and Synthetic Timers.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-counter
uint64_t ArmHyperV::reference_time_100ns() const {
    auto now = clock();
    return std::max(referenceTime, now >= clockOrigin ? now - clockOrigin : 0);
}
// Retry queued messages and integration packets, then service channels and due synthetic timers.
// IMPLEMENTATION NOTE: Ring publication transfers packet ownership; all VPs remain stopped.
// Microsoft TLFS, Inter-Partition Communication: Messages and Event Flags.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication
void ArmHyperV::service() {
    if (enabled) {
        synic.service_message_queues(); // A false injection keeps the queued message for retry.
        if (heartbeatEnabled) {
            auto now = reference_time_100ns();
            auto packet = heartbeat.pending_packet(now);
            if (!packet.empty() &&
                !vmbus.send_channel_packet(ArmHyperVHeartbeat::ChannelId, packet))
                heartbeat.packet_published(now);
        }
        if (timesyncEnabled) {
            auto now = reference_time_100ns();
            auto due = timesync.next_delay_100ns(now);
            if (due && !*due) {
                auto packet = timesync.pending_packet(now, utcClock());
                if (!packet.empty() &&
                    !vmbus.send_channel_packet(ArmHyperVTimeSync::ChannelId, packet))
                    timesync.packet_published(now);
            }
        }
        if (shutdownEnabled) {
            auto now = reference_time_100ns();
            auto packet = shutdown.pending_packet(now);
            if (!packet.empty() && !vmbus.send_channel_packet(ArmHyperVShutdown::ChannelId, packet))
                shutdown.packet_published(now);
        }
        if (kvpEnabled) {
            auto now = reference_time_100ns();
            auto packet = kvp.pending_packet(now);
            if (!packet.empty() && !vmbus.send_channel_packet(ArmHyperVKvp::ChannelId, packet))
                kvp.packet_published(now);
        }
        if (vmbusEnabled)
            vmbus.service_channels();
        timers.service(reference_time_100ns());
    }
}
// Return the next host wakeup delay, or none for idle state; queued work uses a one-millisecond
// floor. IMPLEMENTATION NOTE: Polling/backpressure policy; this does not change wire timer
// deadlines. Microsoft TLFS, Timers: Reference Counter and Synthetic Timers.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-counter
std::optional<uint64_t> ArmHyperV::next_service_delay_100ns() const {
    if (!enabled)
        return {};
    const auto now = reference_time_100ns();
    auto delay = timers.nextDelay(now, false);
    if (heartbeatEnabled) {
        auto next = heartbeat.next_delay_100ns(now);
        if (next)
            delay = std::min(delay.value_or(UINT64_MAX), std::max(*next, MinimumServiceDelay100ns));
    }
    if (timesyncEnabled) {
        auto next = timesync.next_delay_100ns(now);
        if (next)
            delay = std::min(delay.value_or(UINT64_MAX), std::max(*next, MinimumServiceDelay100ns));
    }
    if (shutdownEnabled) {
        auto next = shutdown.next_delay_100ns(now);
        if (next)
            delay = std::min(delay.value_or(UINT64_MAX), std::max(*next, MinimumServiceDelay100ns));
    }
    if (synic.needs_service() || (vmbusEnabled && vmbus.needs_service()))
        return std::min(delay.value_or(MinimumServiceDelay100ns), MinimumServiceDelay100ns);
    if (kvpEnabled) {
        auto next = kvp.next_delay_100ns(now);
        if (next)
            delay = std::min(delay.value_or(UINT64_MAX), std::max(*next, MinimumServiceDelay100ns));
    }
    return delay;
}
// Install native private-interrupt delivery for SynIC/timers; an advertised backend cannot be
// removed. Microsoft TLFS, Inter-Partition Communication: Messages and Event Flags.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication
void ArmHyperV::set_private_interrupt_sender(SoftwareInterruptSender send) {
    if (synicAdvertised && !send)
        throw std::runtime_error("Cannot remove an advertised SynIC interrupt backend");
    privateInterruptSender = std::move(send);
    synic.set_interrupt_sender(privateInterruptSender);
}
// Expose SynIC features only with a real interrupt backend; VMBus prevents later withdrawal.
// Microsoft TLFS, Inter-Partition Communication: Messages and Event Flags.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication
void ArmHyperV::advertise_synic(bool advertise) {
    if (advertise && (!enabled || !privateInterruptSender))
        throw std::runtime_error(
            "SynIC advertisement requires enabled Hyper-V and a private interrupt backend");
    if (!advertise && vmbusEnabled)
        throw std::runtime_error("Cannot disable SynIC with VMBus attached");
    synicAdvertised = advertise;
}
// Bind bootstrap/control message connections and native SynIC event/message delivery once.
// Microsoft TLFS, Inter-Partition Communication: Messages and Event Flags.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication
void ArmHyperV::enable_vmbus() {
    if (vmbusEnabled)
        return;
    if (!enabled || !synicAdvertised || !privateInterruptSender)
        throw std::runtime_error("VMBus requires advertised SynIC with an interrupt backend");
    if (connections.contains(VmbusControlConnection) ||
        connections.contains(VmbusBootstrapConnection) ||
        connections.size() > MaximumConnections - 2)
        throw std::runtime_error("VMBus control connections unavailable");
    vmbus.set_message_sender([this](unsigned vp, unsigned sint, std::span<const uint8_t> payload) {
        return synic.post_message(vp, sint, VmbusMessageType, 0, payload);
    });
    bind_message_connection(VmbusBootstrapConnection,
                            [this](unsigned, uint32_t type, std::span<const uint8_t> payload) {
                                return vmbus.receive_control_message(type, payload, true);
                            });
    bind_message_connection(VmbusControlConnection,
                            [this](unsigned, uint32_t type, std::span<const uint8_t> payload) {
                                return vmbus.receive_control_message(type, payload, false);
                            });
    vmbus.set_event_sender([this](unsigned vp, unsigned sint, unsigned flag) {
        bool newly = false;
        return synic.signal_event(vp, sint, flag, newly);
    });
    // Grant the implemented event API. No channel event ports exist until an
    // actual channel is offered; unknown connections still fail, never succeed.
    allowSignalEvents = true;
    vmbusEnabled = true;
}
// Bind a channel event connection and inventory offer atomically; reject collisions/setup failure.
// Microsoft TLFS, Inter-Partition Communication: Messages and Event Flags.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication
bool ArmHyperV::offer_vmbus_channel(const ArmVmbus::Offer &offer, ArmVmbus::PacketHandler handler,
                                    std::function<void()> closed, std::function<void()> opened) {
    if (!vmbusEnabled || connections.contains(offer.connection))
        return false;
    if (!bind_event_connection(offer.connection, 1, [this, id = offer.channel](unsigned, uint16_t) {
            return vmbus.signal_channel(id);
        }))
        return false;
    if (!vmbus.add_channel(offer, std::move(handler), std::move(closed), std::move(opened))) {
        connections.erase(offer.connection);
        return false;
    }
    vmbusEventPorts.emplace(offer.connection, offer.channel);
    return true;
}
// Offer a heartbeat IC once, wiring open/close/response events to its session.
// Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
void ArmHyperV::enable_heartbeat() {
    if (heartbeatEnabled)
        return;
    if (!offer_vmbus_channel(
            ArmHyperVHeartbeat::channel_offer(),
            [this](std::span<const uint8_t> packet) {
                heartbeat.receive_response(packet, reference_time_100ns());
                return std::vector<uint8_t>{};
            },
            [this] { heartbeat.channel_closed(); },
            [this] { heartbeat.channel_opened(reference_time_100ns()); }))
        throw std::runtime_error("Heartbeat requires VMBus and an available channel/connection");
    heartbeatEnabled = true;
}
// Offer time synchronization once; default host UTC is converted to FILETIME separately from
// monotonic time. Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
void ArmHyperV::enable_time_sync(Clock source) {
    if (timesyncEnabled)
        return;
    if (!source)
        source = [] {
            using Ticks = std::chrono::duration<int64_t, std::ratio<1, ReferenceTicksPerSecond>>;
            return ArmHyperVTimeSync::unix_100ns_to_filetime(
                std::chrono::duration_cast<Ticks>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        };
    if (!offer_vmbus_channel(
            ArmHyperVTimeSync::channel_offer(),
            [this](std::span<const uint8_t> packet) {
                timesync.receive_response(packet, reference_time_100ns());
                return std::vector<uint8_t>{};
            },
            [this] { timesync.channel_closed(); },
            [this] { timesync.channel_opened(reference_time_100ns()); }))
        throw std::runtime_error("Timesync requires VMBus and an available channel/connection");
    utcClock = std::move(source);
    timesyncEnabled = true;
}
// Offer shutdown negotiation once; offering the channel does not request a power action.
// Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
void ArmHyperV::enable_shutdown() {
    if (shutdownEnabled)
        return;
    if (!offer_vmbus_channel(
            ArmHyperVShutdown::channel_offer(),
            [this](std::span<const uint8_t> packet) {
                shutdown.receive_response(packet, reference_time_100ns());
                return std::vector<uint8_t>{};
            },
            [this] { shutdown.channel_closed(); },
            [this] { shutdown.channel_opened(reference_time_100ns()); }))
        throw std::runtime_error("Shutdown requires VMBus and an available channel/connection");
    shutdownEnabled = true;
}
// Offer key/value pair exchange once; no host query or mutation is issued by setup.
// Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
void ArmHyperV::enable_kvp() {
    if (kvpEnabled)
        return;
    if (!offer_vmbus_channel(
            ArmHyperVKvp::channel_offer(),
            [this](std::span<const uint8_t> packet) {
                kvp.receive_response(packet, reference_time_100ns());
                return std::vector<uint8_t>{};
            },
            [this] { kvp.channel_closed(); },
            [this] { kvp.channel_opened(reference_time_100ns()); }))
        throw std::runtime_error("KVP requires VMBus and an available channel/connection");
    kvpEnabled = true;
}
// Queue a trusted host pool/index enumeration only while the enabled KVP session accepts work.
// Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
bool ArmHyperV::enumerate_kvp_value(ArmHyperVKvpCodec::Pool pool, uint32_t index) {
    return enabled && kvpEnabled && kvp.enumerate_value(pool, index);
}
// Queue a trusted host key lookup; return false when unavailable or busy.
// Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
bool ArmHyperV::get_kvp_value(ArmHyperVKvpCodec::Pool pool, const std::u16string &key) {
    return enabled && kvpEnabled && kvp.get_value(pool, key);
}
// Queue an explicit trusted host mutation under the KVP session pool policy.
// Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
bool ArmHyperV::set_kvp_value(ArmHyperVKvpCodec::Pool pool, const std::u16string &key,
                              const ArmHyperVKvpCodec::Value &value) {
    return enabled && kvpEnabled && kvp.set_value(pool, key, value);
}
// Queue an explicit trusted host key deletion under the KVP session pool policy.
// Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
bool ArmHyperV::delete_kvp_value(ArmHyperVKvpCodec::Pool pool, const std::u16string &key) {
    return enabled && kvpEnabled && kvp.delete_value(pool, key);
}
// Transfer the completed KVP result to its host consumer once.
// Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
std::optional<ArmHyperVKvp::Result> ArmHyperV::take_kvp_result() {
    return kvpEnabled ? kvp.take_result() : std::nullopt;
}
// Queue an explicit host power-action request; success means queued, not actual guest shutdown.
// Microsoft TLFS, OpenVMM IC protocol definitions (implementation reference).
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/lib.rs
bool ArmHyperV::request_shutdown(ArmHyperVShutdown::Action action, bool force, uint32_t seconds) {
    return enabled && shutdownEnabled && shutdown.request_shutdown(action, force, seconds);
}
// Bind one bounded, unscoped message connection to a synchronous borrowed-payload callback.
// Microsoft TLFS, Inter-Partition Communication: Messages and Event Flags.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication
bool ArmHyperV::bind_message_connection(uint32_t id, MessageHandler handler) {
    if (id > ConnectionIdMask || !handler || connections.contains(id) ||
        connections.size() >= MaximumConnections)
        return false;
    connections.emplace(id, Connection{std::move(handler), {}, 0});
    allowPostMessages = true;
    return true;
}
// Bind one bounded event connection with an explicit accepted flag range.
// Microsoft TLFS, Inter-Partition Communication: Messages and Event Flags.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication
bool ArmHyperV::bind_event_connection(uint32_t id, unsigned flagCount, EventHandler handler) {
    if (id > ConnectionIdMask || !handler || !flagCount || flagCount > MaximumEventFlags ||
        connections.contains(id) || connections.size() >= MaximumConnections)
        return false;
    connections.emplace(id, Connection{{}, std::move(handler), flagCount});
    allowSignalEvents = true;
    return true;
}
// Remove a host binding except active VMBus-owned control and channel connections.
// Microsoft TLFS, Inter-Partition Communication: Messages and Event Flags.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication
bool ArmHyperV::remove_connection(uint32_t id) {
    if (vmbusEnabled && (id == VmbusControlConnection || id == VmbusBootstrapConnection ||
                         vmbusEventPorts.contains(id)))
        return false;
    return connections.erase(id) != 0;
}
// Find/create a bounded host register-activity counter; count omissions without retaining payloads.
// IMPLEMENTATION NOTE: Diagnostic bookkeeping, not a synthetic register operation.
// Microsoft TLFS, HV_REGISTER_NAME.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name
ArmHyperV::Counts *ArmHyperV::register_counters(uint32_t name) {
    auto it = registers.find(name);
    if (it != registers.end())
        return &it->second;
    if (registers.size() == MaximumDiagnosticEntries) {
        ++omittedCounters;
        return nullptr;
    }
    return &registers[name];
}
// Read one named VP register, route SynIC/timers, and expose only supported discovery features.
// Reference-count reads strictly advance partition time; unavailable registers return an error.
// Microsoft TLFS, HV_REGISTER_NAME.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name
uint16_t ArmHyperV::read_vp_register(uint32_t name, unsigned vp, std::array<uint64_t, 2> &value) {
    value = {};
    if (name >= Register::Timer0ConfigurationRegister && name <= Register::Timer3CountRegister) {
        if (!guestId)
            return AccessDenied;
        return timers.read(name, vp, value[0]);
    }
    if (name >= Register::Sint0Register && name <= Register::SynicResendRegister) {
        if (!guestId)
            return AccessDenied;
        return synic.read_vp_register(name, vp, value[0]);
    }
    if (name >= Register::GuestCrashP0Register && name <= Register::GuestCrashP4Register) {
        value[0] = crashParameters[vp][name - Register::GuestCrashP0Register];
        return 0;
    }
    if (name == Register::GuestCrashControlRegister) {
        value[0] = CrashNotify | CrashMessage;
        return 0;
    }
    switch (name) {
    case Register::HypervisorVersionRegister:
        value[0] = PcEmuVersion;
        return 0; // PCEmu interface version 1.0, build 1
    case Register::PrivilegesAndFeaturesInfoRegister:
        // WinHv requires the complete reference-counter/SynIC/timer/native
        // interrupt-controller/hypercall/VP-index privilege set. On ARM64,
        // interrupt-controller access is the architectural virtual GIC, not
        // the x64-only synthetic EOI/ICR/TPR register interface.
        value[0] = BasePrivileges | (counterProvider ? AccessReferenceTsc : 0) |
                   (synicAdvertised ? AccessSynic : 0) | (vmbusEnabled ? AccessVmbusRegisters : 0) |
                   (allowPostMessages ? PostMessagesPrivilege : 0) |
                   (allowSignalEvents ? SignalEventsPrivilege : 0);
        value[1] = CrashRegistersFeatureHigh;
        return 0; // ARM64 bit 72: crash registers
    case Register::FeaturesInfoRegister:
        value[0] = NoSpinWaitRecommendation | (interruptSender ? RecommendClusterIpi : 0);
        return 0; // Recommend IPIs only with a real interrupt backend
    case Register::ImplementationLimitsInfoRegister:
        value[0] = cpus | (uint64_t(cpus) << 32);
        return 0;
    case Register::HardwareFeaturesInfoRegister:
        value[0] = SecondLevelAddressTranslation;
        return 0; // Stage-2 translation, no synthetic interrupt remapping
    case Register::CpuManagementFeaturesInfoRegister:
    case Register::PasidFeaturesInfoRegister:
    case Register::ReservedFeaturesRegister:
    case Register::NestedVirtFeaturesInfoRegister:
    case Register::IptFeaturesInfoRegister:
    case Register::IsolationConfigurationRegister:
        return 0;
    case Register::GuestOsIdRegister:
        value[0] = guestId;
        return 0;
    case Register::TimeReferenceCountRegister: {
        // Shared by all VPs, including early reads before GuestOsId. Dispatch
        // is serialized with all vCPUs stopped. Clamp equal-resolution samples
        // to maintain the TLFS strictly increasing access order.
        auto now = clock();
        auto elapsed = now >= clockOrigin ? now - clockOrigin : 0;
        referenceTime = std::max(referenceTime + 1, elapsed);
        value[0] = referenceTime;
        return 0;
    }
    case Register::ReferenceTscRegister:
        if (!counterProvider)
            return InvalidParameter;
        if (!guestId)
            return AccessDenied;
        value[0] = referenceClock.registration();
        return 0;
    case Register::InterfaceVersionRegister:
        value[0] = HyperVInterfaceSignature;
        return 0; // ARM64 interface version: Hv#1
    case Register::VpIndexRegister:
        if (!guestId)
            return AccessDenied;
        value[0] = vp;
        return 0;
    case Register::PartitionInfoPageRegister:
        if (!guestId)
            return AccessDenied;
        value[0] = partitionInfo;
        return 0;
    case Register::TlbiControlRegister:
        if (!guestId)
            return AccessDenied;
        value[0] = tlbiControl[vp];
        return 0;
    case Register::SyntheticVectorBaseRegister:
        value[0] = syntheticVectors[vp];
        return 0;
    case Register::SyntheticEsrRegister:
        return 0; // No synthetic exception has been injected.
    default:
        return InvalidParameter;
    }
}
// Validate then update one named VP register or route it to the owning subdevice.
// Snapshot crash data synchronously; invalid page/field writes retain previous state.
// Microsoft TLFS, HV_REGISTER_NAME.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name
uint16_t ArmHyperV::write_vp_register(uint32_t name, unsigned vp, std::array<uint64_t, 2> value) {
    if (name == Register::ReferenceTscRegister) {
        if (!counterProvider)
            return InvalidParameter;
        if (!guestId)
            return AccessDenied;
        if (value[1])
            return InvalidParameter;
        return referenceClock.configure_reference_page(
                   value[0], (value[0] & PageEnabled) ? reference_sample()
                                                      : ArmHyperVReferenceClock::Sample{})
                   ? 0
                   : InvalidParameter;
    }
    if (name >= Register::Timer0ConfigurationRegister && name <= Register::Timer3CountRegister) {
        if (!guestId)
            return AccessDenied;
        if (value[1])
            return InvalidParameter;
        return timers.write(name, vp, value[0], reference_time_100ns());
    }
    if (name >= Register::Sint0Register && name <= Register::SynicResendRegister) {
        if (!guestId)
            return AccessDenied;
        if (value[1])
            return InvalidParameter;
        return synic.write_vp_register(name, vp, value[0]);
    }
    if (name >= Register::GuestCrashP0Register && name <= Register::GuestCrashP4Register) {
        if (value[1])
            return InvalidParameter;
        crashParameters[vp][name - Register::GuestCrashP0Register] = value[0];
        return 0;
    }
    if (name == Register::GuestCrashControlRegister) {
        if (value[1] || (value[0] & CrashReservedLowMask) ||
            (value[0] && !(value[0] & CrashNotify)))
            return InvalidParameter;
        if (!value[0])
            return 0;
        const auto &parameters = crashParameters[vp];
        std::string hex;
        if (value[0] & CrashMessage) {
            auto address = parameters[3], size = parameters[4];
            if (size > MaximumCrashMessageBytes ||
                (size && (address < base || address - base > ram.size() ||
                          size > ram.size() - (address - base))))
                return InvalidParameter;
            constexpr char digits[] = "0123456789abcdef";
            hex.reserve(size_t(size) * 2);
            for (size_t i = 0; i < size; ++i) {
                auto byte = ram[size_t(address - base) + i];
                hex += digits[byte >> 4];
                hex += digits[byte & 15];
            }
        }
        // Copy before returning to the guest: later register/RAM writes must
        // not change already notified diagnostics. No guest shutdown is implied.
        notifiedParameters = parameters;
        crashControl = value[0];
        crashVp = vp;
        crashMessageHex = std::move(hex);
        ++crashNotifications;
        crashPending = true;
        return 0;
    }
    if (name == Register::SyntheticVectorBaseRegister) {
        if (value[1] || (value[0] & SyntheticVectorAlignmentMask))
            return InvalidParameter;
        // A guest VA, separate from the architectural VBAR_EL1. Optional
        // synthetic interrupt/exception producers remain disabled.
        syntheticVectors[vp] = value[0];
        return 0;
    }
    if (name == Register::TlbiControlRegister) {
        if (!guestId)
            return AccessDenied;
        if (value[1] || (value[0] & ~uint64_t(TlbiEnabled)))
            return InvalidParameter;
        // Per-VP TLBI opt-in. Architectural TLBI still executes through HVF;
        // this backend does not queue additional asynchronous flush actions.
        tlbiControl[vp] = value[0];
        return 0;
    }
    if (name == Register::PartitionInfoPageRegister) {
        if (!guestId)
            return AccessDenied;
        if (value[1] || (value[0] & PageReservedMask))
            return InvalidParameter;
        if (value[0] & PageEnabled) {
            uint64_t address = value[0] & ~uint64_t(PageOffsetMask);
            if (address < base || address - base > ram.size() ||
                HypercallPageBytes > ram.size() - (address - base))
                return InvalidParameter;
            // Windows supplies this shared page. No asynchronous address-space
            // flushes are queued by this backend: its pending-flush word is zero.
            // All remaining fields are reserved/zero in this initial interface.
            std::fill_n(ram.begin() + size_t(address - base), HypercallPageBytes, uint8_t(0));
        }
        partitionInfo = value[0];
        return 0;
    }
    if (name != Register::GuestOsIdRegister || value[1])
        return InvalidParameter;
    if (vp != 0)
        return AccessDenied;
    guestId = value[0];
    return 0;
}
// Recognize SMCCC discovery/HVC calls and legacy HVC #1, update bounded diagnostics, and return X0
// status. Return false to leave unsupported instruction/function routing to the machine and PSCI.
// Microsoft TLFS, Hypercall Interface: control words, parameter blocks and repetitions.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
bool ArmHyperV::handle_hypercall(unsigned immediate, Registers &r, unsigned vp) {
    if (immediate > 1)
        return false;
    if (!immediate && uint32_t(r[0]) == VendorHypervisorUidFunction) {
        ++uidQueries;
        if (!enabled)
            return false;
        for (unsigned i = 0; i < HyperVInterfaceUid.size(); ++i)
            r[i] = HyperVInterfaceUid[i];
        return true;
    }
    if (!immediate &&
        (uint32_t(r[0]) == SmcccVersionFunction || uint32_t(r[0]) == SmcccArchFeaturesFunction)) {
        ++versionQueries;
        if (!enabled)
            return false;
        if (uint32_t(r[0]) == SmcccVersionFunction)
            r[0] = SmcccVersion1_2; // SMCCC 1.2
        else
            r[0] = (uint32_t(r[1]) == VendorHypervisorUidFunction ||
                    uint32_t(r[1]) == HyperVFunction || uint32_t(r[1]) == SmcccVersionFunction ||
                    uint32_t(r[1]) == SmcccArchFeaturesFunction)
                       ? 0
                       : uint64_t(-1);
        return true;
    }
    if (!immediate && uint32_t(r[0]) != HyperVFunction)
        return false;
    if (immediate)
        ++legacyCalls;
    else
        ++smcccCalls;
    lastControl = r[immediate ? 0 : 1];
    auto code = uint16_t(lastControl);
    if (calls.contains(code) || calls.size() < MaximumDiagnosticEntries)
        ++calls[code];
    else
        ++omittedCounters;
    if (!enabled) {
        ++disabledCalls;
        lastResult = uint64_t(-1);
        return false;
    }
    if (code == Hypercall::PostMessage) {
        lastPost = {};
        lastPost.observed = true;
        lastPost.control = lastControl;
        lastPost.address = (lastControl & FastHypercall) ? 0 : r[immediate ? 1 : 2];
        lastPost.vp = vp;
    }
    lastResult = invoke_hypercall(r, !immediate, vp);
    if (code == Hypercall::PostMessage)
        lastPost.status = uint16_t(lastResult);
    if (uint16_t(lastResult)) {
        ++failures;
        if (calls.contains(code))
            ++callFailures[code];
    }
    r[0] = lastResult;
    return true;
}
// Validate control/transport/target fields, dispatch an operation and report status or completed
// repetitions. Requests borrow stopped-VM RAM; register writes before a failed repetition remain
// committed. Microsoft TLFS, Hypercall Interface: control words, parameter blocks and repetitions.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
uint64_t ArmHyperV::invoke_hypercall(Registers &r, bool smccc, unsigned vp) {
    auto control = r[smccc ? 1 : 0];
    auto code = uint16_t(control);
    // PostMessage/SignalEvent: validate fixed input, connection type and event flag before
    // delivery.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication
    if (code == Hypercall::PostMessage || code == Hypercall::SignalEvent) {
        if (!guestId || (code == Hypercall::PostMessage ? !allowPostMessages : !allowSignalEvents))
            return AccessDenied;
        if (vp >= cpus || (control & ~uint64_t(SimpleControlMask)))
            return InvalidHypercallInput;
        const bool message = code == Hypercall::PostMessage;
        const bool fast = control & FastHypercall;
        const size_t size = message ? PostMessageBytes : SignalEventBytes;
        const unsigned in = smccc ? 2 : 1;
        std::array<uint8_t, PostMessageBytes> bytes{};
        if (fast) {
            // The fixed PostMessage packet is larger than ARM's 128-byte
            // fast input area. SignalEvent fits in one register pair.
            if (message)
                return InvalidHypercallInput;
            write_little_endian(bytes, 0, r[in], 8);
        } else {
            const auto address = r[in];
            if (address & ParameterAlignmentMask)
                return InvalidAlignment;
            if (size > HypercallPageBytes - (address & PageOffsetMask) || address < base ||
                address - base > ram.size() || size > ram.size() - (address - base))
                return InvalidHypercallInput;
            std::copy_n(ram.begin() + size_t(address - base), size, bytes.begin());
        }
        const auto id = uint32_t(read_little_endian(bytes, MessageField::Connection, 4));
        if (message) {
            const auto type = uint32_t(read_little_endian(bytes, MessageField::Type, 4));
            const auto length = read_little_endian(bytes, MessageField::Size, 4);
            lastPost.packetObserved = true;
            lastPost.connectionId = id;
            lastPost.reserved = uint32_t(read_little_endian(bytes, MessageField::Reserved, 4));
            lastPost.type = type;
            lastPost.size = uint32_t(length);
            // Only the bounded VMBus control prefix, never generic payloads.
            if (vmbusEnabled &&
                ((id & ConnectionIdMask) == VmbusControlConnection ||
                 (id & ConnectionIdMask) == VmbusBootstrapConnection) &&
                type == VmbusMessageType && length <= PostMessagePayloadBytes) {
                constexpr char hex[] = "0123456789abcdef";
                for (size_t i = 0; i < std::min<size_t>(size_t(length), MaximumControlPrefixBytes);
                     ++i) {
                    auto b = bytes[MessageField::Payload + i];
                    lastPost.vmbusPrefixHex += hex[b >> 4];
                    lastPost.vmbusPrefixHex += hex[b & 15];
                }
            }
            // WinHv's reusable PostMessage buffer leaves this alignment word
            // untouched, just like register-association padding.
            if (!type || (type & HypervisorMessageTypeBit) || length > PostMessagePayloadBytes)
                return InvalidParameter;
        } else if (read_little_endian(bytes, EventField::Reserved, 2))
            return InvalidParameter;
        auto found = connections.find(id);
        if (found == connections.end())
            return InvalidConnectionId; // InvalidConnectionId, including unsupported scopes.
        // Copy callbacks so an endpoint can safely remove its own binding.
        auto endpoint = found->second;
        uint16_t status;
        if (message) {
            if (!endpoint.message)
                return InvalidPortId; // InvalidPortId: wrong endpoint type.
            status =
                endpoint.message(vp, uint32_t(read_little_endian(bytes, MessageField::Type, 4)),
                                 std::span<const uint8_t>(bytes).subspan(
                                     MessageField::Payload,
                                     size_t(read_little_endian(bytes, MessageField::Size, 4))));
            if (!status)
                ++postedMessages;
        } else {
            if (!endpoint.event)
                return InvalidPortId;
            const auto flag = uint16_t(read_little_endian(bytes, EventField::Flag, 2));
            if (flag >= endpoint.flagCount)
                return InvalidParameter;
            status = endpoint.event(vp, flag);
            if (!status)
                ++signaledEvents;
        }
        return status;
    }
    // COMPATIBILITY: Windows ARM drain observations are recorded in docs/hyperv-arm64.md; native
    // callback drains this VP.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
    if (code == Hypercall::FlushTlb) {
        // ARM enlightened TLB drain: Windows supplies no input or output
        // parameters, after consulting its partition-info pending-flush word.
        if (!guestId)
            return AccessDenied;
        if ((control & ~uint64_t(SimpleControlMask)) || vp >= cpus)
            return InvalidHypercallInput;
        if (!translationFlusher)
            return InvalidHypercallCode;
        const uint64_t targets = 1ull << vp;
        if (!translationFlusher(targets))
            return OperationFailed;
        ++tlbFlushRequests;
        tlbFlushTargetMask = targets;
        tlbFlushAddressSpace = 0;
        tlbFlushFlags = 0;
        return 0;
    }
    // Flush/IPI/context requests share a VP-mask decoder; extended forms use sparse HV_VP_SET
    // banks.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
    if (code == Hypercall::FlushVirtualAddressSpace ||
        code == Hypercall::FlushVirtualAddressSpaceEx || code == Hypercall::SyncContext ||
        code == Hypercall::SyncContextEx || code == Hypercall::SendSyntheticClusterIpi ||
        code == Hypercall::SendSyntheticClusterIpiEx) {
        if (!guestId)
            return AccessDenied;
        const bool flush = code == Hypercall::FlushVirtualAddressSpace ||
                           code == Hypercall::FlushVirtualAddressSpaceEx;
        const bool ipi = code == Hypercall::SendSyntheticClusterIpi ||
                         code == Hypercall::SendSyntheticClusterIpiEx;
        const bool extended = code == Hypercall::FlushVirtualAddressSpaceEx ||
                              code == Hypercall::SyncContextEx ||
                              code == Hypercall::SendSyntheticClusterIpiEx;
        const uint64_t allowed = extended ? VariableHeaderControlMask : SimpleControlMask;
        if ((control & ~allowed) || vp >= cpus)
            return InvalidHypercallInput;
        size_t banks = extended ? (control >> VariableHeaderShift) & VariableHeaderCountMask : 0;
        if (banks > MaximumVpBanks)
            return InvalidHypercallInput;
        size_t prefix = flush ? 16 : 8;
        size_t size = prefix + (extended ? VpSetHeaderBytes + banks * VpBankBytes : VpBankBytes);
        unsigned in = smccc ? 2 : 1;
        std::vector<uint8_t> input(size);
        if (control & FastHypercall) {
            if (size > FastInputBytes)
                return InvalidHypercallInput;
            for (size_t i = 0; i < size; ++i)
                input[i] = uint8_t(r[in + i / 8] >> ((i % 8) * 8));
        } else {
            auto address = r[in];
            if ((address & ParameterAlignmentMask) ||
                size > HypercallPageBytes - (address & PageOffsetMask) || address < base ||
                address - base > ram.size() || size > ram.size() - (address - base))
                return InvalidAlignment;
            std::copy_n(ram.begin() + size_t(address - base), size, input.begin());
        }
        auto flags = read_little_endian(input, prefix - 8, 8);
        if (flush && (flags & ~uint64_t(FlushFlagsMask)))
            return InvalidParameter;
        // ARM64 IPI vectors are GIC SGI IDs. Accept current VTL0 or explicit
        // VTL0 (UseTargetVtl bit), with all remaining flag/reserved bytes zero.
        if (ipi && ((flags & ~IpiFlagsMask) || uint32_t(flags) >= ArmSoftwareInterruptCount ||
                    ((flags & (UseTargetVtl)) && (flags & (TargetVtlMask)))))
            return InvalidParameter;
        const bool allProcessors = flush && (flags & AllProcessors);
        uint64_t targets = 0;
        const uint64_t valid = (1ull << cpus) - 1;
        if (!extended) {
            targets = read_little_endian(input, prefix, 8);
            if (!flush && !ipi && (flags > 1 || (flags && targets)))
                return InvalidParameter;
            if (allProcessors || (!flush && !ipi && flags))
                targets = valid;
        } else {
            // HV_VP_SET follows the operation's fixed prefix. Only its bank
            // contents belong to the variable input header.
            if (!flush && !ipi && flags)
                return InvalidParameter;
            auto format = read_little_endian(input, prefix, 8),
                 validBanks = read_little_endian(input, prefix + VpBankBytes, 8);
            if (banks != size_t(std::popcount(validBanks)))
                return InvalidHypercallInput;
            if (format == AllVpSet) {
                if (validBanks)
                    return InvalidParameter;
                targets = valid;
            } else if (format == SparseVpSet) {
                size_t at = prefix + VpSetHeaderBytes;
                for (unsigned bank = 0; bank < MaximumVpBanks; ++bank)
                    if (validBanks & (1ull << bank)) {
                        auto bits = read_little_endian(input, at, 8);
                        at += VpBankBytes;
                        // Empty banks are legal even beyond the configured VP range.
                        if (bank && bits && !allProcessors)
                            return InvalidVpIndex;
                        if (!bank)
                            targets = bits;
                    }
            } else
                return InvalidParameter;
        }
        if (allProcessors)
            targets = valid; // The supplied processor mask is ignored.
        if (targets & ~valid)
            return InvalidVpIndex;
        if (ipi) {
            if (!interruptSender)
                return InvalidHypercallCode;
            if (!interruptSender(targets, uint32_t(flags)))
                return OperationFailed;
            ++ipiRequests;
            ipiTargetMask = targets;
            ipiVector = uint32_t(flags);
            for (unsigned cpu = 0; cpu < cpus; ++cpu)
                if (targets & (1ull << cpu))
                    ++ipiTargetRequests[cpu];
            return 0;
        }
        if (flush) {
            if (!translationFlusher)
                return InvalidHypercallCode;
            // The native backend conservatively flushes all ASIDs/global
            // mappings, which also covers any requested address-space subset.
            if (!translationFlusher(targets))
                return OperationFailed;
            ++tlbFlushRequests;
            tlbFlushTargetMask = targets;
            tlbFlushAddressSpace = read_little_endian(input, 0, 8);
            tlbFlushFlags = flags;
            return 0;
        }
        if (!contextSynchronizer)
            return InvalidHypercallCode;
        if (!contextSynchronizer(targets))
            return OperationFailed; // HV_STATUS_OPERATION_FAILED
        ++contextSyncRequests;
        contextSyncTargetMask = targets;
        return 0;
    }
    // HvExtCallQueryCapabilities: zero bitmap truthfully reports no extended services.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
    if (code == Hypercall::QueryExtendedCapabilities) {
        if (!guestId)
            return AccessDenied;
        if ((control & ~uint64_t(SimpleControlMask)) || vp >= cpus)
            return InvalidHypercallInput;
        if (control & FastHypercall) {
            // No input; one 16-byte output pair. Only its low 8 bytes are
            // defined. Clear the padding as for GetVpRegisters fast output.
            unsigned out = smccc ? 2 : 15;
            r[out] = r[out + 1] = 0;
        } else {
            auto address = r[smccc ? 3 : 2];
            if ((address & ParameterAlignmentMask) || address < base ||
                address - base > ram.size() || 8 > ram.size() - (address - base))
                return InvalidAlignment;
            // No 0x8002+ service is currently provided. A zero capability
            // bitmap is the truthful supported-set response, not a stub action.
            std::fill_n(ram.begin() + size_t(address - base), 8, uint8_t(0));
        }
        return 0;
    }
    // GetVpRegisters/SetVpRegisters: validate self/VTL0 and transfer 128-bit register values in
    // list order.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
    if (code != Hypercall::GetVpRegisters && code != Hypercall::SetVpRegisters)
        return guestId ? InvalidHypercallCode : AccessDenied;
    // Only Fast, Code, RepCount and RepStart are supported. No nested/variable headers.
    constexpr uint64_t allowed = RegisterControlMask;
    if (control & ~allowed)
        return InvalidHypercallInput;
    size_t reps = (control >> RepCountShift) & RepCountMask,
           first = (control >> RepStartShift) & RepCountMask;
    if (!reps || first >= reps || vp >= cpus)
        return InvalidHypercallInput;
    bool fast = control & FastHypercall, write = code == Hypercall::SetVpRegisters;
    size_t inputSize =
               RegisterHeaderBytes + reps * (write ? RegisterAssociationBytes : RegisterNameBytes),
           outputSize = write ? 0 : reps * RegisterPairBytes;
    if (inputSize > HypercallPageBytes || outputSize > HypercallPageBytes)
        return InvalidHypercallInput;
    std::vector<uint8_t> input(inputSize);
    std::span<uint8_t> output;
    unsigned inputStart = smccc ? 2 : 1, outputStart = 0;
    auto memory = [&](uint64_t address, size_t size) -> std::span<uint8_t> {
        if ((address & ParameterAlignmentMask) ||
            size > HypercallPageBytes - (address & PageOffsetMask) || address < base ||
            address - base > ram.size() || size > ram.size() - (address - base))
            return {};
        return ram.subspan(size_t(address - base), size);
    };
    if (fast) {
        auto inputPairs = (inputSize + RegisterPairBytes - 1) / RegisterPairBytes,
             outputPairs = outputSize / RegisterPairBytes;
        if (inputPairs + outputPairs > FastInputBytes / RegisterPairBytes)
            return InvalidHypercallInput;
        for (size_t i = 0; i < inputSize; ++i)
            input[i] = uint8_t(r[inputStart + i / 8] >> ((i % 8) * 8));
        // OpenVMM ARM64 ABI: SMCCC output follows 16-byte input pairs;
        // legacy HVC #1 output occupies the final pairs ending at X16.
        outputStart = unsigned(smccc ? inputStart + inputPairs * 2 : 17 - outputPairs * 2);
    } else {
        auto in = memory(r[inputStart], inputSize);
        if (in.empty())
            return InvalidAlignment;
        std::copy(in.begin(), in.end(), input.begin());
        if (outputSize) {
            auto a = r[inputStart], b = r[inputStart + 1];
            output = memory(b, outputSize);
            if (output.empty() || (a < b + outputSize && b < a + inputSize))
                return InvalidAlignment;
        }
    }
    if (read_little_endian(input, RegisterInputField::PartitionId, 8) != SelfPartitionId)
        return AccessDenied;
    auto target = read_little_endian(input, RegisterInputField::VpIndex, 4);
    if (target != SelfVpIndex && target != vp)
        return InvalidVpIndex;
    if (read_little_endian(input, RegisterInputField::VtlAndReserved, 4))
        return InvalidParameter; // VTL0 only, reserved bytes zero
    for (size_t i = first; i < reps; ++i) {
        size_t at =
            RegisterHeaderBytes + i * (write ? RegisterAssociationBytes : RegisterNameBytes);
        auto name = uint32_t(read_little_endian(input, at, 4));
        lastRegister = name;
        auto counters = register_counters(name);
        if (counters) {
            if (write)
                ++counters->writes;
            else
                ++counters->reads;
        }
        std::array<uint64_t, 2> value{};
        uint16_t status;
        if (write) {
            const bool synthetic =
                (name >= Register::Sint0Register && name <= Register::SynicResendRegister) ||
                (name >= Register::Timer0ConfigurationRegister &&
                 name <= Register::Timer3CountRegister);
            if (counters && synthetic) {
                counters->lastWrite = {
                    read_little_endian(input, at + RegisterInputField::ValueLow, 8),
                    read_little_endian(input, at + RegisterInputField::ValueHigh, 8)};
                counters->lastWriteVp = vp;
                counters->lastWriteNamePadding =
                    uint32_t(read_little_endian(input, at + RegisterInputField::NamePadding, 4));
                counters->lastWritePadding =
                    read_little_endian(input, at + RegisterInputField::AlignmentPadding, 8);
                counters->lastWriteFast = fast;
            }
            // Windows WinHv reuses register-association buffers without
            // initializing the 12 alignment bytes. Like OpenVMM, consume only
            // the name and value; header/VTL and register bits stay validated.
            status = write_vp_register(
                name, vp,
                {read_little_endian(input, at + RegisterInputField::ValueLow, 8),
                 read_little_endian(input, at + RegisterInputField::ValueHigh, 8)});
            if (counters && synthetic)
                counters->lastWriteStatus = status;
        } else
            status = read_vp_register(name, vp, value);
        if (status) {
            if (counters)
                ++counters->failures;
            return (uint64_t(i) << RepCountShift) | status;
        }
        if (!write) {
            if (fast) {
                r[outputStart + i * 2] = value[0];
                r[outputStart + i * 2 + 1] = value[1];
            } else {
                write_little_endian(output, i * 16, value[0], 8);
                write_little_endian(output, i * 16 + 8, value[1], 8);
            }
        }
    }
    return uint64_t(reps) << RepCountShift;
}
// Serialize the last immutable crash snapshot as bounded host JSON; do not reread guest RAM.
// IMPLEMENTATION NOTE: Diagnostic format, no guest execution or power action.
// Microsoft TLFS, HV_REGISTER_NAME.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name
std::string ArmHyperV::crash_report() const {
    std::ostringstream s;
    s << "{\"notifications\":" << crashNotifications << ",\"vp\":" << crashVp
      << ",\"control\":" << crashControl << ",\"parameters\":[";
    for (unsigned i = 0; i < 5; ++i) {
        if (i)
            s << ',';
        s << notifiedParameters[i];
    }
    s << "],\"messageHex\":\"" << crashMessageHex << "\"}";
    return s.str();
}
// Drain one pending crash notification; repeated drains return an empty string.
// Microsoft TLFS, HV_REGISTER_NAME.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name
std::string ArmHyperV::take_crash_report() {
    if (!crashPending)
        return {};
    crashPending = false;
    return crash_report();
}
// Serialize host counters, registration state and enabled subdevice reports without changing guest
// state. IMPLEMENTATION NOTE: JSON observability format; payloads are limited to the documented
// control prefix. Microsoft TLFS, HV_REGISTER_NAME.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name
std::string ArmHyperV::report() const {
    std::ostringstream s;
    s << "{\"schema\":1,\"mode\":\""
      << (enabled ? (vmbusEnabled ? "vmbus" : (synicAdvertised ? "synic" : "basic")) : "off")
      << "\",\"guestOsId\":" << guestId << ",\"partitionInfoPage\":" << partitionInfo
      << ",\"referenceTime100ns\":" << referenceTime
      << ",\"referenceTscPage\":" << referenceClock.registration()
      << ",\"referenceTscSequence\":" << referenceClock.sequence()
      << ",\"syntheticTimerDeliveries\":" << timers.delivered()
      << ",\"postedMessages\":" << postedMessages << ",\"signaledEvents\":" << signaledEvents
      << ",\"lastMessagePost\":{\"observed\":" << (lastPost.observed ? "true" : "false")
      << ",\"packetObserved\":" << (lastPost.packetObserved ? "true" : "false")
      << ",\"control\":" << lastPost.control << ",\"address\":" << lastPost.address
      << ",\"vp\":" << lastPost.vp << ",\"status\":" << lastPost.status
      << ",\"connectionId\":" << lastPost.connectionId << ",\"reserved\":" << lastPost.reserved
      << ",\"messageType\":" << lastPost.type << ",\"payloadBytes\":" << lastPost.size
      << ",\"vmbusPrefixHex\":\"" << lastPost.vmbusPrefixHex << "\"}"
      << ",\"ipiRequests\":" << ipiRequests << ",\"ipiTargetMask\":" << ipiTargetMask
      << ",\"ipiVector\":" << ipiVector << ",\"crash\":" << crash_report()
      << ",\"tlbFlushRequests\":" << tlbFlushRequests
      << ",\"tlbFlushTargetMask\":" << tlbFlushTargetMask
      << ",\"tlbFlushAddressSpace\":" << tlbFlushAddressSpace
      << ",\"tlbFlushFlags\":" << tlbFlushFlags
      << ",\"contextSyncRequests\":" << contextSyncRequests
      << ",\"contextSyncTargetMask\":" << contextSyncTargetMask << ",\"uidQueries\":" << uidQueries
      << ",\"smcccVersionQueries\":" << versionQueries << ",\"smcccCalls\":" << smcccCalls
      << ",\"legacyCalls\":" << legacyCalls << ",\"disabledCalls\":" << disabledCalls
      << ",\"failures\":" << failures << ",\"lastControl\":" << lastControl
      << ",\"lastResult\":" << lastResult << ",\"lastRegister\":" << lastRegister
      << ",\"omittedCounters\":" << omittedCounters;
    const auto &activity = timers.activity();
    s << ",\"timerActivity\":{\"configWrites\":" << activity.configWrites
      << ",\"countWrites\":" << activity.countWrites
      << ",\"repeatedConfigWrites\":" << activity.repeatedConfigWrites
      << ",\"repeatedCountWrites\":" << activity.repeatedCountWrites
      << ",\"zeroCountWrites\":" << activity.zeroCountWrites
      << ",\"autoEnableWrites\":" << activity.autoEnableWrites
      << ",\"periodicRearms\":" << activity.periodicRearms
      << ",\"unchangedArmedDeadline\":" << activity.unchangedArmedDeadline
      << ",\"serviceCalls\":" << timers.serviceCalls()
      << ",\"serviceTimerVisits\":" << timers.serviceTimerVisits()
      << ",\"deadlineQueries\":" << timers.deadlineQueries()
      << ",\"deadlineTimerVisits\":" << timers.deadlineTimerVisits()
      << ",\"deadlineRebuilds\":" << timers.deadlineRebuilds()
      << ",\"serviceFastReturns\":" << timers.serviceFastReturns()
      << ",\"deliveryRetries\":" << timers.deliveryRetries() << ",\"configFlags\":[";
    for (unsigned i = 0; i < activity.configFlags.size(); ++i) {
        if (i)
            s << ',';
        s << activity.configFlags[i];
    }
    s << "]},\"calls\":[";
    bool comma = false;
    for (auto [code, n] : calls) {
        if (comma)
            s << ',';
        comma = true;
        s << "{\"code\":" << code << ",\"count\":" << n
          << ",\"failures\":" << (callFailures.contains(code) ? callFailures.at(code) : 0) << '}';
    }
    s << "],\"ipiTargetRequests\":[";
    for (unsigned cpu = 0; cpu < cpus; ++cpu) {
        if (cpu)
            s << ',';
        s << ipiTargetRequests[cpu];
    }
    s << "],\"registers\":[";
    comma = false;
    for (auto [name, n] : registers) {
        if (comma)
            s << ',';
        comma = true;
        s << "{\"name\":" << name << ",\"reads\":" << n.reads << ",\"writes\":" << n.writes
          << ",\"failures\":" << n.failures;
        if (n.writes &&
            ((name >= Register::Sint0Register && name <= Register::SynicResendRegister) ||
             (name >= Register::Timer0ConfigurationRegister &&
              name <= Register::Timer3CountRegister)))
            s << ",\"lastWriteLow\":" << n.lastWrite[0] << ",\"lastWriteHigh\":" << n.lastWrite[1]
              << ",\"lastWriteVp\":" << n.lastWriteVp
              << ",\"lastWriteStatus\":" << n.lastWriteStatus
              << ",\"lastWriteNamePadding\":" << n.lastWriteNamePadding
              << ",\"lastWritePadding\":" << n.lastWritePadding
              << ",\"lastWriteFast\":" << (n.lastWriteFast ? "true" : "false");
        s << '}';
    }
    s << "],\"vps\":[";
    for (unsigned vp = 0; vp < cpus; ++vp) {
        if (vp)
            s << ',';
        s << "{\"index\":" << vp << ",\"tlbiControl\":" << tlbiControl[vp]
          << ",\"syntheticVector\":" << syntheticVectors[vp] << '}';
    }
    s << ']';
    if (synicAdvertised)
        s << ",\"synic\":" << synic.report();
    if (vmbusEnabled)
        s << ",\"vmbus\":" << vmbus.report();
    if (kvpEnabled)
        s << ",\"kvp\":" << kvp.report();
    if (shutdownEnabled)
        s << ",\"shutdown\":" << shutdown.report();
    if (timesyncEnabled)
        s << ",\"timesync\":" << timesync.report();
    if (heartbeatEnabled)
        s << ",\"heartbeat\":" << heartbeat.report();
    return s.str() + "}\n";
}
} // namespace pc
